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
#pragma once

#include <vtpty/Pty.h>
#include <vtpty/UnixUtils.h>

#include <crispy/BufferObject.h>
#include <crispy/read_selector.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

#if defined(__APPLE__)
    #include <util.h>
#elif defined(__linux__)
    #include <pty.h>
#endif

namespace terminal
{

class UnixPty final: public Pty
{
  private:
    class Slave final: public PtySlave
    {
        int _slaveFd;

      public:
        explicit Slave(PtySlaveHandle fd): _slaveFd { unbox<int>(fd) } {}
        ~Slave() override;
        [[nodiscard]] PtySlaveHandle handle() const noexcept;
        void close() override;
        [[nodiscard]] bool isClosed() const noexcept override;
        bool configure() noexcept override;
        bool login() override;
        int write(std::string_view) noexcept override;
    };

  public:
    struct PtyHandles
    {
        PtyMasterHandle master;
        PtySlaveHandle slave;
    };

    UnixPty(PageSize pageSize, std::optional<crispy::image_size> pixels);
    ~UnixPty() override;

    PtySlave& slave() noexcept override;

    [[nodiscard]] PtyMasterHandle handle() const noexcept;
    void start() override;
    void close() override;
    [[nodiscard]] bool isClosed() const noexcept override;
    void wakeupReader() noexcept override;
    [[nodiscard]] ReadResult read(crispy::buffer_object<char>& storage,
                                  std::chrono::milliseconds timeout,
                                  size_t size) override;
    int write(std::string_view data) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<crispy::image_size> pixels = std::nullopt) override;

    UnixPipe& stdoutFastPipe() noexcept { return _stdoutFastPipe; }

  private:
    std::optional<std::string_view> readSome(int fd, char* target, size_t n) noexcept;

    [[nodiscard]] bool started() const noexcept { return _masterFd != -1; }

    int _masterFd = -1;
    UnixPipe _stdoutFastPipe;
    crispy::read_selector _readSelector;
    PageSize _pageSize;
    std::optional<crispy::image_size> _pixels;
    std::unique_ptr<Slave> _slave;
};

} // namespace terminal
