/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
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
#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>
#include <contour/display/TerminalWidget.h>
#include <contour/helper.h>

#include <vtbackend/MatchModes.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/ViCommands.h>

#include <vtpty/Process.h>
#include <vtpty/Pty.h>

#include <crispy/StackTrace.h>

#include <QtCore/QDebug>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QMimeData>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtNetwork/QHostInfo>

#include <algorithm>
#include <fstream>
#include <limits>

#include "vtbackend/primitives.h"

#if !defined(_WIN32)
    #include <pthread.h>
#endif

#if !defined(_MSC_VER)
    #include <csignal>

    #include <unistd.h>
#endif

#if defined(_MSC_VER)
    #define __PRETTY_FUNCTION__ __FUNCDNAME__
#endif

using std::chrono::steady_clock;
using namespace std;
using namespace terminal;

namespace contour
{

namespace
{
    string unhandledExceptionMessage(string_view const& where, exception const& e)
    {
        return fmt::format("{}: Unhandled exception caught ({}). {}", where, typeid(e).name(), e.what());
    }

    void setThreadName(char const* name)
    {
#if defined(__APPLE__)
        pthread_setname_np(name);
#elif !defined(_WIN32)
        pthread_setname_np(pthread_self(), name);
#endif
    }

    string normalize_crlf(QString&& text)
    {
#if !defined(_WIN32)
        return text.replace("\r\n", "\n").replace("\r", "\n").toUtf8().toStdString();
#else
        return text.toUtf8().toStdString();
#endif
    }

    string strip_if(string input, bool shouldStrip)
    {
        if (!shouldStrip)
            return input;

        string output = input;

        do
        {
            std::swap(input, output);
            output = crispy::replace(input, "  ", " ");
            output = crispy::replace(output, "\t", " ");
            output = crispy::replace(output, "\n", " ");
        } while (input != output);

        return output;
    }

    terminal::settings createSettingsFromConfig(config::Config const& config,
                                                config::TerminalProfile const& profile)
    {
        auto settings = terminal::settings {};

        settings.ptyBufferObjectSize = config.ptyBufferObjectSize;
        settings.ptyReadBufferSize = config.ptyReadBufferSize;
        settings.maxHistoryLineCount = profile.maxHistoryLineCount;
        settings.copyLastMarkRangeOffset = profile.copyLastMarkRangeOffset;
        settings.cursorBlinkInterval = profile.inputModes.insert.cursor.cursorBlinkInterval;
        settings.wordDelimiters = unicode::from_utf8(config.wordDelimiters);
        settings.mouseProtocolBypassModifier = config.bypassMouseProtocolModifier;
        settings.maxImageSize = config.maxImageSize;
        settings.maxImageRegisterCount = config.maxImageColorRegisters;
        settings.statusDisplayType = profile.initialStatusDisplayType;
        settings.statusDisplayPosition = profile.statusDisplayPosition;
        settings.syncWindowTitleWithHostWritableStatusDisplay =
            profile.syncWindowTitleWithHostWritableStatusDisplay;
        settings.colorPalette = profile.colors;
        settings.refreshRate = profile.refreshRate;
        settings.primaryScreen.allowReflowOnResize = config.reflowOnResize;
        settings.highlightDoubleClickedWord = profile.highlightDoubleClickedWord;
        settings.highlightTimeout = profile.highlightTimeout;

        return settings;
    }

    int createSessionId()
    {
        static int nextSessionId = 1;
        return nextSessionId++;
    }

} // namespace

TerminalSession::TerminalSession(unique_ptr<Pty> _pty, ContourGuiApp& _app):
    id_ { createSessionId() },
    startTime_ { steady_clock::now() },
    config_ { _app.config() },
    profileName_ { _app.profileName() },
    profile_ { *config_.profile(profileName_) },
    app_ { _app },
    terminal_ {
        *this, std::move(_pty), createSettingsFromConfig(config_, profile_), std::chrono::steady_clock::now()
    }
{
    if (_app.liveConfig())
    {
        SessionLog()("Enable live configuration reloading of file {}.",
                     config_.backingFilePath.generic_string());
        configFileChangeWatcher_ = make_unique<QFileSystemWatcher>();
        configFileChangeWatcher_->addPath(QString::fromStdString(config_.backingFilePath.generic_string()));
        connect(configFileChangeWatcher_.get(),
                SIGNAL(fileChanged(const QString&)),
                this,
                SLOT(onConfigReload()));
    }
    musicalNotesBuffer_.reserve(16);
    profile_ = *config_.profile(profileName_); // XXX do it again. but we've to be more efficient here
    configureTerminal();
}

TerminalSession::~TerminalSession()
{
    SessionLog()("Destroying terminal session.");
    terminating_ = true;
    terminal_.device().wakeupReader();
    if (screenUpdateThread_)
        screenUpdateThread_->join();
}

void TerminalSession::detachDisplay(display::TerminalWidget& display)
{
    SessionLog()("Detaching display from session.");
    Require(display_ == &display);
    display_ = nullptr;
}

void TerminalSession::attachDisplay(display::TerminalWidget& newDisplay)
{
    SessionLog()("Attaching display.");
    // newDisplay.setSession(*this); // NB: we're being called by newDisplay!
    display_ = &newDisplay;

    setContentScale(newDisplay.contentScale());

    // NB: Inform connected TTY and local Screen instance about initial cell pixel size.
    auto const pixels = display_->cellSize() * terminal_.pageSize();
    // auto const pixels =
    //     ImageSize { display_->cellSize().width * boxed_cast<Width>(terminal_.pageSize().columns),
    //                 display_->cellSize().height * boxed_cast<Height>(terminal_.pageSize().lines) };
    terminal_.resizeScreen(terminal_.pageSize(), pixels);
    terminal_.setRefreshRate(display_->refreshRate());
}

void TerminalSession::scheduleRedraw()
{
    terminal_.markScreenDirty();
    if (display_)
        display_->scheduleRedraw();
}

void TerminalSession::start()
{
    terminal_.device().start();
    screenUpdateThread_ = make_unique<std::thread>(bind(&TerminalSession::mainLoop, this));
}

void TerminalSession::mainLoop()
{
    setThreadName("Terminal.Loop");

    mainLoopThreadID_ = this_thread::get_id();

    SessionLog()("Starting main loop with thread id {}", [&]() {
        stringstream sstr;
        sstr << mainLoopThreadID_;
        return sstr.str();
    }());

    while (!terminating_)
    {
        if (!terminal_.processInputOnce())
            break;
    }

    SessionLog()("Event loop terminating (PTY {}).", terminal_.device().isClosed() ? "closed" : "open");
    onClosed();
}

void TerminalSession::terminate()
{
    if (!display_)
        return;

    display_->closeDisplay();
}

// {{{ Events implementations
void TerminalSession::bell()
{
    emit onBell();
}

void TerminalSession::bufferChanged(terminal::screen_type _type)
{
    if (!display_)
        return;

    currentScreenType_ = _type;
    emit isScrollbarVisibleChanged();
    display_->post([this, _type]() { display_->bufferChanged(_type); });
}

void TerminalSession::screenUpdated()
{
    if (!display_)
        return;

    if (profile_.autoScrollOnUpdate && terminal().get_viewport().scrolled()
        && terminal().inputHandler().mode() == vi_mode::Insert)
        terminal().get_viewport().scrollToBottom();

    if (terminal().hasInput())
        display_->post(bind(&TerminalSession::flushInput, this));

    if (lastHistoryLineCount_ != terminal_.currentScreen().historyLineCount())
    {
        lastHistoryLineCount_ = terminal_.currentScreen().historyLineCount();
        emit historyLineCountChanged(unbox<int>(lastHistoryLineCount_));
    }

    scheduleRedraw();
}

void TerminalSession::flushInput()
{
    terminal().flushInput();
    if (terminal().hasInput() && display_)
        display_->post(bind(&TerminalSession::flushInput, this));
}

void TerminalSession::renderBufferUpdated()
{
    if (!display_)
        return;

    display_->renderBufferUpdated();
}

void TerminalSession::executeRole(GuardedRole role, bool allow, bool remember)
{
    switch (role)
    {
        case GuardedRole::CaptureBuffer: executePendingBufferCapture(allow, remember); break;
        case GuardedRole::ChangeFont: applyPendingFontChange(allow, remember); break;
        case GuardedRole::ShowHostWritableStatusLine:
            executeShowHostWritableStatusLine(allow, remember);
            break;
    }
}

void TerminalSession::requestPermission(config::Permission _allowedByConfig, GuardedRole role)
{
    switch (_allowedByConfig)
    {
        case config::Permission::Allow:
            SessionLog()("Permission for {} allowed by configuration.", role);
            executeRole(role, true, false);
            return;
        case config::Permission::Deny:
            SessionLog()("Permission for {} denied by configuration.", role);
            executeRole(role, false, false);
            return;
        case config::Permission::Ask: {
            if (auto const i = rememberedPermissions_.find(role); i != rememberedPermissions_.end())
            {
                executeRole(role, i->second, false);
                if (!i->second)
                    SessionLog()("Permission for {} denied by user for this session.", role);
                else
                    SessionLog()("Permission for {} allowed by user for this session.", role);
            }
            else
            {
                SessionLog()("Permission for {} requires asking user.", role);
                switch (role)
                {
                        // clang-format off
                    case GuardedRole::ChangeFont: emit requestPermissionForFontChange(); break;
                    case GuardedRole::CaptureBuffer: emit requestPermissionForBufferCapture(); break;
                    case GuardedRole::ShowHostWritableStatusLine: emit requestPermissionForShowHostWritableStatusLine(); break;
                        // clang-format on
                }
            }
            break;
        }
    }
}

void TerminalSession::requestCaptureBuffer(LineCount lines, bool logical)
{
    if (!display_)
        return;

    pendingBufferCapture_ = CaptureBufferRequest { lines, logical };

    emit requestPermissionForBufferCapture();
    // display_->post(
    //     [this]() { requestPermission(profile_.permissions.captureBuffer, GuardedRole::CaptureBuffer); });
}

void TerminalSession::executePendingBufferCapture(bool allow, bool remember)
{
    if (remember)
        rememberedPermissions_[GuardedRole::CaptureBuffer] = allow;

    if (!pendingBufferCapture_)
        return;

    auto const capture = std::move(pendingBufferCapture_.value());
    pendingBufferCapture_.reset();

    if (!allow)
        return;

    terminal_.primaryScreen().captureBuffer(capture.lines, capture.logical);

    DisplayLog()("requestCaptureBuffer: Finished. Waking up I/O thread.");
    flushInput();
}

void TerminalSession::executeShowHostWritableStatusLine(bool allow, bool remember)
{
    if (remember)
        rememberedPermissions_[GuardedRole::ShowHostWritableStatusLine] = allow;

    if (!allow)
        return;

    terminal_.setStatusDisplay(terminal::status_display_type::HostWritable);
    DisplayLog()("requestCaptureBuffer: Finished. Waking up I/O thread.");
    flushInput();
    terminal_.state().syncWindowTitleWithHostWritableStatusDisplay = false;
}

terminal::font_def TerminalSession::getFontDef()
{
    return display_->getFontDef();
}

void TerminalSession::setFontDef(terminal::font_def const& _fontDef)
{
    if (!display_)
        return;

    pendingFontChange_ = _fontDef;

    display_->post([this]() { requestPermission(profile_.permissions.changeFont, GuardedRole::ChangeFont); });
}

void TerminalSession::applyPendingFontChange(bool allow, bool remember)
{
    if (remember)
        rememberedPermissions_[GuardedRole::ChangeFont] = allow;

    if (!pendingFontChange_)
        return;

    auto const& currentFonts = profile_.fonts;
    terminal::rasterizer::FontDescriptions newFonts = currentFonts;

    auto const spec = std::move(pendingFontChange_.value());
    pendingFontChange_.reset();

    if (!allow)
        return;

    if (spec.size != 0.0)
        newFonts.size = text::font_size { spec.size };

    if (!spec.regular.empty())
        newFonts.regular = text::font_description::parse(spec.regular);

    auto const styledFont = [&](string_view _font) -> text::font_description {
        // if a styled font is "auto" then infer froom regular font"
        if (_font == "auto"sv)
            return currentFonts.regular;
        else
            return text::font_description::parse(_font);
    };

    if (!spec.bold.empty())
        newFonts.bold = styledFont(spec.bold);

    if (!spec.italic.empty())
        newFonts.italic = styledFont(spec.italic);

    if (!spec.boldItalic.empty())
        newFonts.boldItalic = styledFont(spec.boldItalic);

    if (!spec.emoji.empty() && spec.emoji != "auto"sv)
        newFonts.emoji = text::font_description::parse(spec.emoji);

    display_->setFonts(newFonts);
}

void TerminalSession::copyToClipboard(std::string_view _data)
{
    if (!display_)
        return;

    display_->post([this, data = string(_data)]() { display_->copyToClipboard(data); });
}

void TerminalSession::inspect()
{
    if (display_)
        display_->inspect();

    // Deferred termination? Then close display now.
    if (terminal_.device().isClosed() && !app_.dumpStateAtExit().has_value())
        display_->closeDisplay();
}

void TerminalSession::notify(string_view _title, string_view _content)
{
    emit showNotification(QString::fromUtf8(_title.data(), static_cast<int>(_title.size())),
                          QString::fromUtf8(_content.data(), static_cast<int>(_content.size())));
}

void TerminalSession::onClosed()
{
    auto const now = steady_clock::now();
    auto const diff = std::chrono::duration_cast<std::chrono::seconds>(now - startTime_);

    if (auto const* localProcess = dynamic_cast<terminal::Process const*>(&terminal_.device()))
    {
        auto const exitStatus = localProcess->checkStatus();
        if (exitStatus)
            SessionLog()(
                "Process terminated after {} seconds with exit status {}.", diff.count(), *exitStatus);
        else
            SessionLog()("Process terminated after {} seconds.", diff.count());
    }
    else
        SessionLog()("Process terminated after {} seconds.", diff.count());

    emit sessionClosed(*this);

    if (diff < app_.earlyExitThreshold())
    {
        // auto const w = terminal_.pageSize().columns.as<int>();
        auto constexpr SGR = "\033[1;38:2::255:255:255m\033[48:2::255:0:0m"sv;
        auto constexpr EL = "\033[K"sv;
        auto constexpr TextLines = array<string_view, 2> { "Shell terminated too quickly.",
                                                           "The window will not be closed automatically." };
        for (auto const text: TextLines)
            terminal_.writeToScreen(fmt::format("\r\n{}{}{}", SGR, EL, text));
        terminal_.writeToScreen("\r\n");
        terminatedAndWaitingForKeyPress_ = true;
        return;
    }

    if (app_.dumpStateAtExit().has_value())
        inspect();
    else if (display_)
        display_->closeDisplay();
}

void TerminalSession::pasteFromClipboard(unsigned count, bool strip)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        QMimeData const* md = clipboard->mimeData();
        SessionLog()("pasteFromClipboard: mime data contains {} formats.", md->formats().size());
        for (int i = 0; i < md->formats().size(); ++i)
            SessionLog()("pasteFromClipboard[{}]: {}\n", i, md->formats().at(i).toStdString());
        string const text = strip_if(normalize_crlf(clipboard->text(QClipboard::Clipboard)), strip);
        if (text.empty())
            SessionLog()("Clipboard does not contain text.");
        else if (count == 1)
            terminal().sendPaste(string_view { text });
        else
        {
            string fullPaste;
            for (unsigned i = 0; i < count; ++i)
                fullPaste += text;
            terminal().sendPaste(string_view { fullPaste });
        }
    }
    else
        SessionLog()("Could not access clipboard.");
}

void TerminalSession::onSelectionCompleted()
{
    switch (config_.onMouseSelection)
    {
        case config::SelectionAction::CopyToSelectionClipboard:
            if (QClipboard* clipboard = QGuiApplication::clipboard();
                clipboard != nullptr && clipboard->supportsSelection())
            {
                string const text = terminal().extractSelectionText();
                clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())),
                                   QClipboard::Selection);
            }
            break;
        case config::SelectionAction::CopyToClipboard:
            if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
            {
                string const text = terminal().extractSelectionText();
                clipboard->setText(QString::fromUtf8(text.c_str(), static_cast<int>(text.size())),
                                   QClipboard::Clipboard);
            }
            break;
        case config::SelectionAction::Nothing: break;
    }
}

void TerminalSession::requestWindowResize(LineCount _lines, ColumnCount _columns)
{
    if (!display_)
        return;

    SessionLog()("Application request to resize window: {}x{} px", _columns, _lines);
    display_->post([this, _lines, _columns]() { display_->resizeWindow(_lines, _columns); });
}

void TerminalSession::requestWindowResize(QJSValue w, QJSValue h)
{
    requestWindowResize(width(w.toInt()), height(h.toInt()));
}

void TerminalSession::requestWindowResize(width _width, height _height)
{
    if (!display_)
        return;

    SessionLog()("Application request to resize window: {}x{} px", _width, _height);
    display_->post([this, _width, _height]() { display_->resizeWindow(_width, _height); });
}

void TerminalSession::setWindowTitle(string_view _title)
{
    emit titleChanged(QString::fromUtf8(_title.data(), static_cast<int>(_title.size())));
}

void TerminalSession::setTerminalProfile(string const& _configProfileName)
{
    if (!display_)
        return;

    display_->post([this, name = string(_configProfileName)]() { activateProfile(name); });
}

void TerminalSession::discardImage(terminal::image const& _image)
{
    if (!display_)
        return;

    display_->discardImage(_image);
}

void TerminalSession::inputModeChanged(terminal::vi_mode mode)
{
    using terminal::vi_mode;
    switch (mode)
    {
        case vi_mode::Insert: configureCursor(profile_.inputModes.insert.cursor); break;
        case vi_mode::Normal: configureCursor(profile_.inputModes.normal.cursor); break;
        case vi_mode::Visual:
        case vi_mode::VisualLine:
        case vi_mode::VisualBlock: configureCursor(profile_.inputModes.visual.cursor); break;
    }
}

// }}}
// {{{ Input Events
void TerminalSession::sendKeyPressEvent(key _key, modifier _modifier, Timestamp _now)
{
    InputLog()("Key press event received: {} {}", _modifier, _key);

    if (terminatedAndWaitingForKeyPress_)
    {
        display_->closeDisplay();
        return;
    }

    if (profile_.mouse_hide_while_typing)
        display_->setMouseCursorShape(MouseCursorShape::Hidden);

    if (auto const* actions =
            config::apply(config_.inputMappings.keyMappings, _key, _modifier, matchModeFlags()))
        executeAllActions(*actions);
    else
        terminal().sendKeyPressEvent(_key, _modifier, _now);
}

void TerminalSession::sendCharPressEvent(char32_t _value, modifier _modifier, Timestamp _now)
{
    InputLog()("Character press event received: {} {}",
               _modifier,
               crispy::escape(unicode::convert_to<char>(_value)));

    assert(display_ != nullptr);

    if (terminatedAndWaitingForKeyPress_)
    {
        display_->closeDisplay();
        return;
    }

    if (profile_.mouse_hide_while_typing)
        display_->setMouseCursorShape(MouseCursorShape::Hidden);

    if (auto const* actions =
            config::apply(config_.inputMappings.charMappings, _value, _modifier, matchModeFlags()))
        executeAllActions(*actions);
    else
        terminal().sendCharPressEvent(_value, _modifier, _now); // TODO: get rid of Event{} struct here, too!
}

void TerminalSession::sendMousePressEvent(modifier _modifier,
                                          mouse_button _button,
                                          pixel_coordinate _pixelPosition)
{
    auto const uiHandledHint = false;
    InputLog()("Mouse press received: {} {}\n", _modifier, _button);

    terminal().tick(steady_clock::now());

    if (terminal().sendMousePressEvent(_modifier, _button, _pixelPosition, uiHandledHint))
        return;

    auto const modifier = _modifier.contains(config_.bypassMouseProtocolModifier)
                              ? _modifier.without(config_.bypassMouseProtocolModifier)
                              : _modifier;

    if (auto const* actions =
            config::apply(config_.inputMappings.mouseMappings, _button, modifier, matchModeFlags()))
        executeAllActions(*actions);
}

void TerminalSession::sendMouseMoveEvent(terminal::modifier _modifier,
                                         terminal::cell_location _pos,
                                         terminal::pixel_coordinate _pixelPosition)
{
    // NB: This translation depends on the display's margin, so maybe
    //     the display should provide the translation?

    if (!(_pos < terminal().pageSize()))
        return;

    terminal().tick(steady_clock::now());

    auto constexpr uiHandledHint = false;
    terminal().sendMouseMoveEvent(_modifier, _pos, _pixelPosition, uiHandledHint);

    if (_pos != currentMousePosition_)
    {
        // Change cursor shape only when changing grid cell.
        currentMousePosition_ = _pos;
        if (terminal().isMouseHoveringHyperlink())
            display_->setMouseCursorShape(MouseCursorShape::PointingHand);
        else
            setDefaultCursor();
    }
}

void TerminalSession::sendMouseReleaseEvent(modifier _modifier,
                                            mouse_button _button,
                                            pixel_coordinate _pixelPosition)
{
    terminal().tick(steady_clock::now());

    auto const uiHandledHint = false;
    terminal().sendMouseReleaseEvent(_modifier, _button, _pixelPosition, uiHandledHint);
    scheduleRedraw();
}

void TerminalSession::sendFocusInEvent()
{
    // as per Qt-documentation, some platform implementations reset the cursor when leaving the
    // window, so we have to re-apply our desired cursor in focusInEvent().
    setDefaultCursor();

    terminal().sendFocusInEvent();

    if (display_)
        display_->setBlurBehind(profile_.backgroundBlur);

    scheduleRedraw();
}

void TerminalSession::sendFocusOutEvent()
{
    // TODO maybe paint with "faint" colors
    terminal().sendFocusOutEvent();

    scheduleRedraw();
}

void TerminalSession::updateHighlights()
{
    QTimer::singleShot(terminal().highlightTimeout(), this, SLOT(onHighlightUpdate()));
}

void TerminalSession::onHighlightUpdate()
{
    terminal_.resetHighlight();
}

void TerminalSession::playSound(terminal::sequence::parameters const& params_)
{
    auto range = params_.range();
    musicalNotesBuffer_.clear();
    musicalNotesBuffer_.insert(musicalNotesBuffer_.begin(), range.begin() + 2, range.end());
    emit audio.play(params_.at(0), params_.at(1), musicalNotesBuffer_);
}

void TerminalSession::cursorPositionChanged()
{
    QGuiApplication::inputMethod()->update(Qt::ImCursorRectangle);
}
// }}}
// {{{ Actions
bool TerminalSession::operator()(actions::CancelSelection)
{
    terminal_.clearSelection();
    return true;
}

bool TerminalSession::operator()(actions::ChangeProfile const& _action)
{
    SessionLog()("Changing profile to: {}", _action.name);
    if (_action.name == profileName_)
        return true;

    activateProfile(_action.name);
    return true;
}

bool TerminalSession::operator()(actions::ClearHistoryAndReset)
{
    SessionLog()("Clearing history and perform terminal hard reset");

    terminal_.hardReset();
    terminal_.forceRedraw([]() { this_thread::yield(); });
    return true;
}

bool TerminalSession::operator()(actions::CopyPreviousMarkRange)
{
    copyToClipboard(terminal().extractLastMarkRange());
    return true;
}

bool TerminalSession::operator()(actions::CopySelection copySelection)
{
    switch (copySelection.format)
    {
        case actions::CopyFormat::Text:
            // Copy the selection in pure text, plus whitespaces and newline.
            copyToClipboard(terminal().extractSelectionText());
            break;
        case actions::CopyFormat::HTML:
            // TODO: This requires walking through each selected cell and construct HTML+CSS for it.
        case actions::CopyFormat::VT:
            // TODO: Construct VT escape sequences.
        case actions::CopyFormat::PNG:
            // TODO: Copy to clipboard as rendered PNG for the selected area.
            errorlog()("CopySelection format {} is not yet supported.", copySelection.format);
            return false;
    }
    return true;
}

bool TerminalSession::operator()(actions::CreateDebugDump)
{
    terminal_.inspect();
    return true;
}

bool TerminalSession::operator()(actions::DecreaseFontSize)
{
    auto constexpr OnePt = text::font_size { 1.0 };
    setFontSize(profile().fonts.size - OnePt);
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize - OnePt;
    // setFontSize(newFontSize);
    return true;
}

bool TerminalSession::operator()(actions::DecreaseOpacity)
{
    if (static_cast<uint8_t>(profile_.backgroundOpacity) == 0)
        return true;

    --profile_.backgroundOpacity;

    emit opacityChanged();

    // Also emit backgroundColorChanged() because the background color is
    // semi-transparent and thus the opacity change affects the background color.
    emit backgroundColorChanged();

    return true;
}

bool TerminalSession::operator()(actions::FocusNextSearchMatch)
{
    if (terminal_.state().viCommands.jumpToNextMatch(1))
        terminal_.get_viewport().makeVisibleWithinSafeArea(terminal_.state().viCommands.cursorPosition.line);
    // TODO why didn't the makeVisibleWithinSafeArea() call from inside jumpToNextMatch not work?
    return true;
}

bool TerminalSession::operator()(actions::FocusPreviousSearchMatch)
{
    if (terminal_.state().viCommands.jumpToPreviousMatch(1))
        terminal_.get_viewport().makeVisibleWithinSafeArea(terminal_.state().viCommands.cursorPosition.line);
    // TODO why didn't the makeVisibleWithinSafeArea() call from inside jumpToPreviousMatch not work?
    return true;
}

bool TerminalSession::operator()(actions::FollowHyperlink)
{
    auto const _l = scoped_lock { terminal() };
    if (auto const hyperlink = terminal().tryGetHoveringHyperlink())
    {
        followHyperlink(*hyperlink);
        return true;
    }
    return false;
}

bool TerminalSession::operator()(actions::IncreaseFontSize)
{
    auto constexpr OnePt = text::font_size { 1.0 };
    // auto const currentFontSize = view().renderer().fontDescriptions().size;
    // auto const newFontSize = currentFontSize + OnePt;
    // setFontSize(newFontSize);
    setFontSize(profile().fonts.size + OnePt);
    return true;
}

bool TerminalSession::operator()(actions::IncreaseOpacity)
{
    if (static_cast<uint8_t>(profile_.backgroundOpacity) >= std::numeric_limits<uint8_t>::max())
        return true;
    ++profile_.backgroundOpacity;

    emit opacityChanged();

    // Also emit backgroundColorChanged() because the background color is
    // semi-transparent and thus the opacity change affects the background color.
    emit backgroundColorChanged();

    return true;
}

bool TerminalSession::operator()(actions::NewTerminal const& _action)
{
    spawnNewTerminal(_action.profileName.value_or(profileName_));
    return true;
}

bool TerminalSession::operator()(actions::NoSearchHighlight)
{
    terminal_.clearSearch();
    return true;
}

bool TerminalSession::operator()(actions::OpenConfiguration)
{
    if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(config_.backingFilePath.string().c_str()))))
        errorlog()("Could not open configuration file \"{}\".", config_.backingFilePath.generic_string());

    return true;
}

bool TerminalSession::operator()(actions::OpenFileManager)
{
    auto const _l = scoped_lock { terminal() };
    auto const& cwd = terminal().currentWorkingDirectory();
    if (!QDesktopServices::openUrl(QUrl(QString::fromUtf8(cwd.c_str()))))
        errorlog()("Could not open file \"{}\".", cwd);

    return true;
}

bool TerminalSession::operator()(actions::PasteClipboard paste)
{
    pasteFromClipboard(1, paste.strip);
    return true;
}

bool TerminalSession::operator()(actions::PasteSelection)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        string const text = normalize_crlf(clipboard->text(QClipboard::Selection));
        terminal().sendPaste(string_view { text });
    }

    return true;
}

bool TerminalSession::operator()(actions::Quit)
{
    // TODO: later warn here when more then one terminal view is open
    terminal().device().close();
    exit(EXIT_SUCCESS);
}

bool TerminalSession::operator()(actions::ReloadConfig const& _action)
{
    if (_action.profileName.has_value())
        reloadConfigWithProfile(_action.profileName.value());
    else
        reloadConfigWithProfile(profileName_);

    return true;
}

bool TerminalSession::operator()(actions::ResetConfig)
{
    resetConfig();
    return true;
}

bool TerminalSession::operator()(actions::ResetFontSize)
{
    if (config::TerminalProfile const* profile = config_.profile(profileName_))
        setFontSize(profile->fonts.size);
    return true;
}

bool TerminalSession::operator()(actions::ScreenshotVT)
{
    auto _l = lock_guard { terminal() };
    auto const screenshot = terminal().isPrimaryScreen() ? terminal().primaryScreen().screenshot()
                                                         : terminal().alternateScreen().screenshot();
    ofstream ofs { "screenshot.vt", ios::trunc | ios::binary };
    ofs << screenshot;
    return true;
}

bool TerminalSession::operator()(actions::ScrollDown)
{
    terminal().get_viewport().scrollDown(profile_.historyScrollMultiplier);
    return true;
}

bool TerminalSession::operator()(actions::ScrollMarkDown)
{
    terminal().get_viewport().scrollMarkDown();
    return true;
}

bool TerminalSession::operator()(actions::ScrollMarkUp)
{
    terminal().get_viewport().scrollMarkUp();
    return true;
}

bool TerminalSession::operator()(actions::ScrollOneDown)
{
    terminal().get_viewport().scrollDown(LineCount(1));
    return true;
}

bool TerminalSession::operator()(actions::ScrollOneUp)
{
    terminal().get_viewport().scrollUp(LineCount(1));
    return true;
}

bool TerminalSession::operator()(actions::ScrollPageDown)
{
    auto const stepSize = terminal().pageSize().lines / LineCount(2);
    terminal().get_viewport().scrollDown(stepSize);
    return true;
}

bool TerminalSession::operator()(actions::ScrollPageUp)
{
    auto const stepSize = terminal().pageSize().lines / LineCount(2);
    terminal().get_viewport().scrollUp(stepSize);
    return true;
}

bool TerminalSession::operator()(actions::ScrollToBottom)
{
    terminal().get_viewport().scrollToBottom();
    return true;
}

bool TerminalSession::operator()(actions::ScrollToTop)
{
    terminal().get_viewport().scrollToTop();
    return true;
}

bool TerminalSession::operator()(actions::ScrollUp)
{
    terminal().get_viewport().scrollUp(profile_.historyScrollMultiplier);
    return true;
}

bool TerminalSession::operator()(actions::SearchReverse)
{
    terminal_.inputHandler().startSearchExternally();

    return true;
}

bool TerminalSession::operator()(actions::SendChars const& event)
{
    // auto const now = steady_clock::now();
    // for (auto const ch: event.chars)
    //     terminal().sendCharPressEvent(static_cast<char32_t>(ch), terminal::Modifier::None, now);
    terminal().sendRawInput(event.chars);
    return true;
}

bool TerminalSession::operator()(actions::ToggleAllKeyMaps)
{
    allowKeyMappings_ = !allowKeyMappings_;
    InputLog()("{} key mappings.", allowKeyMappings_ ? "Enabling" : "Disabling");
    return true;
}

bool TerminalSession::operator()(actions::ToggleFullscreen)
{
    if (display_)
        display_->toggleFullScreen();
    return true;
}

bool TerminalSession::operator()(actions::ToggleInputProtection)
{
    terminal().setAllowInput(!terminal().allowInput());
    return true;
}

bool TerminalSession::operator()(actions::ToggleStatusLine)
{
    auto const _l = scoped_lock { terminal_ };
    if (terminal().state().statusDisplayType != status_display_type::Indicator)
        terminal().setStatusDisplay(status_display_type::Indicator);
    else
        terminal().setStatusDisplay(status_display_type::None);

    // `savedStatusDisplayType` holds only a value if the application has been overriding
    // the status display type. But the user now actively requests a given type,
    // so make sure restoring will not destroy the user's desire.
    if (terminal().state().savedStatusDisplayType)
        terminal().state().savedStatusDisplayType = terminal().state().statusDisplayType;

    return true;
}

bool TerminalSession::operator()(actions::ToggleTitleBar)
{
    if (display_)
        display_->toggleTitleBar();
    return true;
}

// {{{ Trace debug mode
bool TerminalSession::operator()(actions::TraceBreakAtEmptyQueue)
{
    terminal_.setExecutionMode(execution_mode::BreakAtEmptyQueue);
    return true;
}

bool TerminalSession::operator()(actions::TraceEnter)
{
    terminal_.setExecutionMode(execution_mode::Waiting);
    return true;
}

bool TerminalSession::operator()(actions::TraceLeave)
{
    terminal_.setExecutionMode(execution_mode::Normal);
    return true;
}

bool TerminalSession::operator()(actions::TraceStep)
{
    terminal_.setExecutionMode(execution_mode::SingleStep);
    return true;
}
// }}}

bool TerminalSession::operator()(actions::ViNormalMode)
{
    if (terminal().inputHandler().mode() == vi_mode::Insert)
        terminal().inputHandler().setMode(vi_mode::Normal);
    else if (terminal().inputHandler().mode() == vi_mode::Normal)
        terminal().inputHandler().setMode(vi_mode::Insert);
    return true;
}

bool TerminalSession::operator()(actions::WriteScreen const& _event)
{
    terminal().writeToScreen(_event.chars);
    return true;
}
// }}}
// {{{ implementation helpers
void TerminalSession::setDefaultCursor()
{
    if (!display_)
        return;

    using Type = terminal::screen_type;
    switch (terminal().screenType())
    {
        case Type::Primary: display_->setMouseCursorShape(MouseCursorShape::IBeam); break;
        case Type::Alternate: display_->setMouseCursorShape(MouseCursorShape::Arrow); break;
    }
}

bool TerminalSession::reloadConfig(config::Config _newConfig, string const& _profileName)
{
    // clang-format off
    SessionLog()("Reloading configuration from {} with profile {}",
                 _newConfig.backingFilePath.string(), _profileName);
    // clang-format on

    config_ = std::move(_newConfig);
    activateProfile(_profileName);

    return true;
}

int TerminalSession::executeAllActions(std::vector<actions::Action> const& _actions)
{
    if (allowKeyMappings_)
    {
        int executionCount = 0;
        for (actions::Action const& action: _actions)
            if (executeAction(action))
                ++executionCount;
        scheduleRedraw();
        return executionCount;
    }

    auto const containsToggleKeybind = [](std::vector<actions::Action> const& _actions) {
        return std::any_of(_actions.begin(), _actions.end(), [](actions::Action const& action) {
            return holds_alternative<actions::ToggleAllKeyMaps>(action);
        });
    };

    if (containsToggleKeybind(_actions))
    {
        bool const ex = executeAction(actions::ToggleAllKeyMaps {});
        scheduleRedraw();
        return ex;
    }

    InputLog()("Key mappings are currently disabled via ToggleAllKeyMaps input mapping action.");
    return 0;
}

// Executes given action @p _action.
//
// The return value indicates whether or not this action did apply or not.
// For example a FollowHyperlink only applies when there is a hyperlink
// at the current cursor position to follow,
// however, a ScrollToTop applies regardless of the current viewport
// scrolling position.
bool TerminalSession::executeAction(actions::Action const& _action)
{
    SessionLog()("executeAction: {}", _action);
    return visit(*this, _action);
}

void TerminalSession::spawnNewTerminal(string const& _profileName)
{
    auto const wd = [this]() -> string {
#if !defined(_WIN32)
        if (auto const* ptyProcess = dynamic_cast<Process const*>(&terminal_.device()))
            return ptyProcess->workingDirectory();
#else
        auto const _l = scoped_lock { terminal_ };
        return terminal_.currentWorkingDirectory();
#endif
        return "."s;
    }();

    if (config_.spawnNewProcess)
    {
        SessionLog()("spawning new process");
        ::contour::spawnNewTerminal(
            app_.programPath(), config_.backingFilePath.generic_string(), _profileName, wd);
    }
    else
    {
        SessionLog()("spawning new in-process window");
        app_.config().profile(profileName_)->shell.workingDirectory = FileSystem::path(wd);
        app_.newWindow();
    }
}

void TerminalSession::activateProfile(string const& _newProfileName)
{
    auto newProfile = config_.profile(_newProfileName);
    if (!newProfile)
    {
        SessionLog()("Cannot change profile. No such profile: '{}'.", _newProfileName);
        return;
    }

    SessionLog()("Changing profile to {}.", _newProfileName);
    profileName_ = _newProfileName;
    profile_ = *newProfile;
    configureTerminal();
    configureDisplay();
}

void TerminalSession::configureTerminal()
{
    auto const _l = scoped_lock { terminal_ };
    SessionLog()("Configuring terminal.");

    terminal_.setWordDelimiters(config_.wordDelimiters);
    terminal_.setMouseProtocolBypassModifier(config_.bypassMouseProtocolModifier);
    terminal_.setMouseBlockSelectionModifier(config_.mouseBlockSelectionModifier);
    terminal_.setLastMarkRangeOffset(profile_.copyLastMarkRangeOffset);

    SessionLog()("Setting terminal ID to {}.", profile_.terminalId);
    terminal_.setTerminalId(profile_.terminalId);
    terminal_.setMaxImageColorRegisters(config_.maxImageColorRegisters);
    terminal_.setMaxImageSize(config_.maxImageSize);
    terminal_.setMode(terminal::dec_mode::NoSixelScrolling, !config_.sixelScrolling);
    terminal_.setStatusDisplay(profile_.initialStatusDisplayType);
    SessionLog()("maxImageSize={}, sixelScrolling={}", config_.maxImageSize, config_.sixelScrolling);

    // XXX
    // if (!_terminalView.renderer().renderTargetAvailable())
    //     return;

    configureCursor(profile_.inputModes.insert.cursor);
    terminal_.colorPalette() = profile_.colors;
    terminal_.defaultColorPalette() = profile_.colors;
    terminal_.setMaxHistoryLineCount(profile_.maxHistoryLineCount);
    terminal_.setHighlightTimeout(profile_.highlightTimeout);
    terminal_.get_viewport().setScrollOff(profile_.modalCursorScrollOff);
}

void TerminalSession::configureCursor(config::CursorConfig const& cursorConfig)
{
    terminal_.setCursorBlinkingInterval(cursorConfig.cursorBlinkInterval);
    terminal_.setCursorDisplay(cursorConfig.cursorDisplay);
    terminal_.setCursorShape(cursorConfig.cursorShape);

    // Force a redraw of the screen
    // to ensure the correct cursor shape is displayed.
    scheduleRedraw();
}

void TerminalSession::configureDisplay()
{
    if (!display_)
        return;

    SessionLog()("Configuring display.");
    display_->setBlurBehind(profile_.backgroundBlur);

    {
        auto const dpr = display_->contentScale();
        auto const qActualScreenSize = display_->window()->screen()->size() * dpr;
        auto const actualScreenSize = image_size { width::cast_from(qActualScreenSize.width()),
                                                   height::cast_from(qActualScreenSize.height()) };
        terminal_.setMaxImageSize(actualScreenSize, actualScreenSize);
    }

    if (profile_.maximized)
        display_->setWindowMaximized();
    else
        display_->setWindowNormal();

    if (profile_.fullscreen != display_->isFullScreen())
        display_->toggleFullScreen();

    terminal_.setRefreshRate(display_->refreshRate());
    auto const pageSize = PageSize {
        LineCount(unbox<int>(display_->pixelSize().height) / unbox<int>(display_->cellSize().height)),
        ColumnCount(unbox<int>(display_->pixelSize().width) / unbox<int>(display_->cellSize().width)),
    };
    display_->setPageSize(pageSize);
    display_->setFonts(profile_.fonts);
    // TODO: maybe update margin after this call?

    display_->setHyperlinkDecoration(profile_.hyperlinkDecoration.normal, profile_.hyperlinkDecoration.hover);

    setWindowTitle(terminal_.windowTitle());
}

uint8_t TerminalSession::matchModeFlags() const
{
    uint8_t flags = 0;

    if (terminal_.isAlternateScreen())
        flags |= static_cast<uint8_t>(match_modes::flag::alternate_screen);

    if (terminal_.applicationCursorKeys())
        flags |= static_cast<uint8_t>(match_modes::flag::app_cursor);

    if (terminal_.applicationKeypad())
        flags |= static_cast<uint8_t>(match_modes::flag::app_keypad);

    if (terminal_.selectionAvailable())
        flags |= static_cast<uint8_t>(match_modes::flag::select);

    if (terminal_.inputHandler().mode() == vi_mode::Insert)
        flags |= static_cast<uint8_t>(match_modes::flag::insert);

    if (!terminal_.state().searchMode.pattern.empty())
        flags |= static_cast<uint8_t>(match_modes::flag::search);

    if (terminal_.executionMode() != execution_mode::Normal)
        flags |= static_cast<uint8_t>(match_modes::flag::trace);

    return flags;
}

void TerminalSession::setFontSize(text::font_size _size)
{
    if (!display_->setFontSize(_size))
        return;

    profile_.fonts.size = _size;
}

bool TerminalSession::reloadConfigWithProfile(string const& _profileName)
{
    auto newConfig = config::Config {};
    auto configFailures = int { 0 };

    try
    {
        loadConfigFromFile(newConfig, config_.backingFilePath.string());
    }
    catch (exception const& e)
    {
        // TODO: logger_.error(e.what());
        errorlog()("Configuration failure. {}", unhandledExceptionMessage(__PRETTY_FUNCTION__, e));
        ++configFailures;
    }

    if (!newConfig.profile(_profileName))
    {
        errorlog()(fmt::format("Currently active profile with name '{}' gone.", _profileName));
        ++configFailures;
    }

    if (configFailures)
    {
        errorlog()("Failed to load configuration.");
        return false;
    }

    return reloadConfig(std::move(newConfig), _profileName);
}

bool TerminalSession::resetConfig()
{
    auto const ec = config::createDefaultConfig(config_.backingFilePath);
    if (ec)
    {
        errorlog()("Failed to load default config at {}; ({}) {}",
                   config_.backingFilePath.string(),
                   ec.category().name(),
                   ec.message());
        return false;
    }

    config::Config defaultConfig;
    try
    {
        config::loadConfigFromFile(config_.backingFilePath);
    }
    catch (exception const& e)
    {
        SessionLog()("Failed to load default config: {}", e.what());
    }

    return reloadConfig(defaultConfig, defaultConfig.defaultProfileName);
}

void TerminalSession::followHyperlink(terminal::hyperlink_info const& _hyperlink)
{
    auto const fileInfo = QFileInfo(QString::fromStdString(string(_hyperlink.path())));
    auto const isLocal =
        _hyperlink.isLocal() && _hyperlink.host() == QHostInfo::localHostName().toStdString();
    auto const editorEnv = getenv("EDITOR");

    if (isLocal && fileInfo.isFile() && fileInfo.isExecutable())
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(config_.backingFilePath.string()));
        args.append(QString::fromUtf8(_hyperlink.path().data(), static_cast<int>(_hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(app_.programPath()), args);
    }
    else if (isLocal && fileInfo.isFile() && editorEnv && *editorEnv)
    {
        QStringList args;
        args.append("config");
        args.append(QString::fromStdString(config_.backingFilePath.string()));
        args.append(QString::fromStdString(editorEnv));
        args.append(QString::fromUtf8(_hyperlink.path().data(), static_cast<int>(_hyperlink.path().size())));
        QProcess::execute(QString::fromStdString(app_.programPath()), args);
    }
    else if (isLocal)
        QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(string(_hyperlink.path()).c_str())));
    else
        QDesktopServices::openUrl(QString::fromUtf8(_hyperlink.uri.c_str()));
}

void TerminalSession::onConfigReload()
{
    display_->post([this]() { reloadConfigWithProfile(profileName_); });

    // TODO: needed still?
    // if (setScreenDirty())
    //     update();

    if (configFileChangeWatcher_)
        connect(configFileChangeWatcher_.get(),
                SIGNAL(fileChanged(const QString&)),
                this,
                SLOT(onConfigReload()));
}

// }}}
// {{{ QAbstractItemModel impl
QModelIndex TerminalSession::index(int row, int column, const QModelIndex& parent) const
{
    Require(row == 0);
    Require(column == 0);
    // NOTE: if at all, we could expose session attribs like session id, session type
    // (local process), ...?
    crispy::ignore_unused(parent);
    return createIndex(row, column, nullptr);
}

QModelIndex TerminalSession::parent(const QModelIndex& child) const
{
    crispy::ignore_unused(child);
    return QModelIndex();
}

int TerminalSession::rowCount(const QModelIndex& parent) const
{
    crispy::ignore_unused(parent);
    return 1;
}

int TerminalSession::columnCount(const QModelIndex& parent) const
{
    crispy::ignore_unused(parent);
    return 1;
}

QVariant TerminalSession::data(const QModelIndex& index, int role) const
{
    crispy::ignore_unused(index, role);
    Require(index.row() == 0);
    Require(index.column() == 0);

    return QVariant(id_);
}

bool TerminalSession::setData(const QModelIndex& index, const QVariant& value, int role)
{
    // NB: Session-Id is read-only.
    crispy::ignore_unused(index, value, role);
    return false;
}
// }}}

} // namespace contour
