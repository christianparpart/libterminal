/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <vtpty/UnixPty.h>
#include <vtpty/UnixUtils.h>

#include <crispy/BufferObject.h>
#include <crispy/deferred.h>
#include <crispy/escape.h>
#include <crispy/logstore.h>

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(__APPLE__)
    #include <util.h>
#elif defined(__FreeBSD__)
    #include <libutil.h>
#else
    #include <pty.h>
#endif

#include <fcntl.h>
#if !defined(__FreeBSD__)
    #include <utmp.h>
#endif
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <pwd.h>
#include <unistd.h>

#if defined(__linux__) && !defined(FLATPAK)
    #include <utempter.h>
#endif

using std::make_unique;
using std::max;
using std::min;
using std::nullopt;
using std::numeric_limits;
using std::optional;
using std::runtime_error;
using std::scoped_lock;
using std::string_view;
using std::tuple;

using namespace std::string_literals;

namespace terminal
{

namespace
{
    int waitForReadable(int ptyMaster,
                        int stdoutFastPipe,
                        int wakeupPipe,
                        std::chrono::milliseconds timeout) noexcept
    {
        if (ptyMaster < 0)
        {
            if (PtyInLog)
                PtyInLog()("read() called with closed PTY master.");
            errno = ENODEV;
            return -1;
        }

        auto tv = timeval {};
        tv.tv_sec = timeout.count() / 1000;
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);

        for (;;)
        {
            fd_set rfd;
            fd_set wfd;
            fd_set efd;
            FD_ZERO(&rfd);
            FD_ZERO(&wfd);
            FD_ZERO(&efd);
            if (ptyMaster != -1)
                FD_SET(ptyMaster, &rfd);
            if (stdoutFastPipe != -1)
                FD_SET(stdoutFastPipe, &rfd);
            FD_SET(wakeupPipe, &rfd);
            auto const nfds = 1 + max(max(ptyMaster, stdoutFastPipe), wakeupPipe);

            int rv = select(nfds, &rfd, &wfd, &efd, &tv);
            if (rv == 0)
            {
                // (Let's not be too verbose here.)
                // PtyInLog()("PTY read() timed out.");
                errno = EAGAIN;
                return -1;
            }

            if (ptyMaster < 0)
            {
                errno = ENODEV;
                return -1;
            }

            if (rv < 0)
            {
                PtyInLog()("PTY read() failed. {}", strerror(errno));
                return -1;
            }

            bool piped = false;
            if (FD_ISSET(wakeupPipe, &rfd))
            {
                piped = true;
                for (bool done = false; !done;)
                {
                    char dummy[256];
                    rv = static_cast<int>(::read(wakeupPipe, dummy, sizeof(dummy)));
                    done = rv > 0;
                }
            }

            if (stdoutFastPipe != -1 && FD_ISSET(stdoutFastPipe, &rfd))
                return stdoutFastPipe;

            if (FD_ISSET(ptyMaster, &rfd))
                return ptyMaster;

            if (piped)
            {
                errno = EINTR;
                return -1;
            }
        }
    }

    UnixPty::PtyHandles createUnixPty(PageSize const& windowSize, optional<crispy::image_size> pixels)
    {
        // See https://code.woboq.org/userspace/glibc/login/forkpty.c.html
        assert(*windowSize.lines <= numeric_limits<unsigned short>::max());
        assert(*windowSize.columns <= numeric_limits<unsigned short>::max());

        winsize const ws { unbox<unsigned short>(windowSize.lines),
                           unbox<unsigned short>(windowSize.columns),
                           unbox<unsigned short>(pixels.value_or(crispy::image_size {}).width),
                           unbox<unsigned short>(pixels.value_or(crispy::image_size {}).height) };

#if defined(__APPLE__)
        auto* wsa = const_cast<winsize*>(&ws);
#else
        winsize const* wsa = &ws;
#endif

        // TODO: termios term{};
        int masterFd {};
        int slaveFd {};
        if (openpty(&masterFd, &slaveFd, nullptr, /*&term*/ nullptr, (winsize*) wsa) < 0)
            throw runtime_error { "Failed to open PTY. "s + strerror(errno) };

        PtyLog()("PTY opened. master={}, slave={}", masterFd, slaveFd);

        return { PtyMasterHandle::cast_from(masterFd), PtySlaveHandle::cast_from(slaveFd) };
    }

#if defined(__linux__) && !defined(FLATPAK)
    char const* hostnameForUtmp()
    {
        for (auto const* env: { "DISPLAY", "WAYLAND_DISPLAY" })
            if (auto const* value = std::getenv(env))
                return value;

        return nullptr;
    }
#endif
} // namespace

// {{{ UnixPty::Slave
UnixPty::Slave::~Slave()
{
    close();
}

PtySlaveHandle UnixPty::Slave::handle() const noexcept
{
    return PtySlaveHandle::cast_from(_slaveFd);
}

void UnixPty::Slave::close()
{
    detail::saveClose(&_slaveFd);
}

bool UnixPty::Slave::isClosed() const noexcept
{
    return _slaveFd == -1;
}

bool UnixPty::Slave::configure() noexcept
{
    auto const tio = detail::constructTerminalSettings(_slaveFd);
    if (tcsetattr(_slaveFd, TCSANOW, &tio) == 0)
        tcflush(_slaveFd, TCIOFLUSH);
    return true;
}

bool UnixPty::Slave::login()
{
    if (_slaveFd < 0)
        return false;

    if (!configure())
        return false;

    // This is doing what login_tty() is doing, too.
    // But doing it ourselfs allows for a little more flexibility.
    // return login_tty(_slaveFd) == 0;

    setsid();

#if defined(TIOCSCTTY)
    if (ioctl(_slaveFd, TIOCSCTTY, nullptr) == -1)
        return false;
#endif

    for (int const fd: { 0, 1, 2 })
    {
        if (_slaveFd != fd)
            ::close(fd);
        detail::saveDup2(_slaveFd, fd);
    }

    if (_slaveFd > 2)
        detail::saveClose(&_slaveFd);

    return true;
}

int UnixPty::Slave::write(std::string_view text) noexcept
{
    if (_slaveFd < 0)
    {
        errno = ENODEV;
        return -1;
    }

    auto const rv = ::write(_slaveFd, text.data(), text.size());
    return static_cast<int>(rv);
}
// }}}

UnixPty::UnixPty(PageSize pageSize, optional<crispy::image_size> pixels):
    _pageSize { pageSize }, _pixels { pixels }
{
}

void UnixPty::start()
{
    auto const handles = createUnixPty(_pageSize, _pixels);
    _masterFd = unbox<int>(handles.master);
    _slave = make_unique<Slave>(handles.slave);

    if (!detail::setFileFlags(_masterFd, O_CLOEXEC | O_NONBLOCK))
        throw runtime_error { "Failed to configure PTY. "s + strerror(errno) };

    detail::setFileFlags(_stdoutFastPipe.reader(), O_NONBLOCK);
    PtyLog()("stdout fastpipe: reader {}, writer {}", _stdoutFastPipe.reader(), _stdoutFastPipe.writer());

    _readSelector.want_read(_masterFd);
    _readSelector.want_read(_stdoutFastPipe.reader());

#if defined(__linux__) && !defined(FLATPAK)
    utempter_add_record(_masterFd, hostnameForUtmp());
#endif
}

UnixPty::~UnixPty()
{
    PtyLog()("PTY destroying master (file descriptor {}).", _masterFd);
    detail::saveClose(&_masterFd);
}

PtySlave& UnixPty::slave() noexcept
{
    assert(started());
    return *_slave;
}

PtyMasterHandle UnixPty::handle() const noexcept
{
    return PtyMasterHandle::cast_from(_masterFd);
}

void UnixPty::close()
{
    PtyLog()("PTY closing master (file descriptor {}).", _masterFd);
    detail::saveClose(&_masterFd);
    wakeupReader();
}

bool UnixPty::isClosed() const noexcept
{
    return _masterFd == -1;
}

void UnixPty::wakeupReader() noexcept
{
    _readSelector.wakeup();
}

optional<string_view> UnixPty::readSome(int fd, char* target, size_t n) noexcept
{
    auto const rv = static_cast<int>(::read(fd, target, n));
    if (rv < 0)
    {
        if (errno != EAGAIN && errno != EINTR)
            errorlog()("{} read failed: {}", fd == _masterFd ? "master" : "stdout-fastpipe", strerror(errno));
        return nullopt;
    }

    if (PtyInLog)
        PtyInLog()("{} received: \"{}\"",
                   fd == _masterFd ? "master" : "stdout-fastpipe",
                   crispy::escape(target, target + rv));

    if (rv == 0 && fd == _stdoutFastPipe.reader())
    {
        PtyInLog()("Closing stdout-fastpipe.");
        _stdoutFastPipe.closeReader();
        errno = EAGAIN;
        return nullopt;
    }

    return string_view { target, static_cast<size_t>(rv) };
}

Pty::ReadResult UnixPty::read(crispy::buffer_object<char>& storage,
                              std::chrono::milliseconds timeout,
                              size_t size)
{
    if (auto const fd = _readSelector.wait_one(timeout); fd.has_value())
    {
        auto const _l = scoped_lock { storage };
        if (auto x = readSome(*fd, storage.hotEnd(), min(size, storage.bytesAvailable())))
            return { tuple { x.value(), *fd == _stdoutFastPipe.reader() } };
    }

    return nullopt;
}

int UnixPty::write(std::string_view data)
{
    auto const* buf = data.data();
    auto const size = data.size();

    ssize_t rv = ::write(_masterFd, buf, size);
    if (PtyOutLog)
    {
        if (rv >= 0)
            PtyOutLog()("Sending bytes: \"{}\"", crispy::escape(buf, buf + rv));

        if (rv < 0)
            // errorlog()("PTY write failed: {}", strerror(errno));
            PtyOutLog()("PTY write of {} bytes failed. {}\n", size, strerror(errno));
        else if (0 <= rv && static_cast<size_t>(rv) < size)
            // clang-format off
            PtyOutLog()("Partial write. {} bytes written and {} bytes left.",
                        rv,
                        size - static_cast<size_t>(rv));
        // clang-format on
    }

    if (0 <= rv && static_cast<size_t>(rv) < size)
    {
        detail::setFileBlocking(_masterFd, true);
        auto const rv2 = ::write(_masterFd, buf + rv, size - rv);
        detail::setFileBlocking(_masterFd, false);
        if (rv2 >= 0)
        {
            if (PtyOutLog)
                PtyOutLog()("Sending bytes: \"{}\"", crispy::escape(buf + rv, buf + rv + rv2));
            return static_cast<int>(rv + rv2);
        }
    }

    return static_cast<int>(rv);
}

PageSize UnixPty::pageSize() const noexcept
{
    return _pageSize;
}

void UnixPty::resizeScreen(PageSize cells, std::optional<crispy::image_size> pixels)
{
    if (_masterFd < 0)
        return;

    auto w = winsize {};
    w.ws_col = unbox<unsigned short>(cells.columns);
    w.ws_row = unbox<unsigned short>(cells.lines);

    if (pixels.has_value())
    {
        w.ws_xpixel = unbox<unsigned short>(pixels.value().width);
        w.ws_ypixel = unbox<unsigned short>(pixels.value().height);
    }

    if (ioctl(_masterFd, TIOCSWINSZ, &w) == -1)
        throw runtime_error { strerror(errno) };

    _pageSize = cells;
}

} // namespace terminal
