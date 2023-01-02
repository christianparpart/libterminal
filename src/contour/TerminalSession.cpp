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
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtGui/QWindow>
#include <QtNetwork/QHostInfo>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

#include <algorithm>
#include <fstream>

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

    terminal::Settings createSettingsFromConfig(config::Config const& config,
                                                config::TerminalProfile const& profile)
    {
        auto settings = terminal::Settings {};

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
        settings.colorPalette = profile.colors;
        settings.refreshRate = profile.refreshRate;
        settings.primaryScreen.allowReflowOnResize = config.reflowOnResize;
        settings.highlightDoubleClickedWord = profile.highlightDoubleClickedWord;
        settings.highlightTimeout = profile.highlightTimeout;

        return settings;
    }

} // namespace

TerminalSession::TerminalSession(unique_ptr<Pty> _pty, ContourGuiApp& _app):
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
    terminating_ = true;
    terminal_.device().wakeupReader();
    if (screenUpdateThread_)
        screenUpdateThread_->join();
}

void TerminalSession::attachDisplay(display::TerminalWidget& newDisplay)
{
    SessionLog()("Attaching display.");
    newDisplay.setSession(*this);
    display_ = &newDisplay;

    setContentScale(newDisplay.contentScale());

    // NB: Inform connected TTY and local Screen instance about initial cell pixel size.
    auto const pixels = display_->cellSize() * terminal_.pageSize();
    // auto const pixels =
    //     ImageSize { display_->cellSize().width * boxed_cast<Width>(terminal_.pageSize().columns),
    //                 display_->cellSize().height * boxed_cast<Height>(terminal_.pageSize().lines) };
    terminal_.resizeScreen(terminal_.pageSize(), pixels);
    terminal_.setRefreshRate(display_->refreshRate());
    configureDisplay();
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
    SessionLog()("TODO: Beep!");
    QApplication::beep();
    // QApplication::beep() requires Qt Widgets dependency. doesn't suound good.
    // so maybe just a visual bell then? That would require additional OpenGL/shader work then though.
}

void TerminalSession::bufferChanged(terminal::ScreenType _type)
{
    if (!display_)
        return;

    display_->post([this, _type]() { display_->bufferChanged(_type); });
}

void TerminalSession::screenUpdated()
{
    if (!display_)
        return;

    if (profile_.autoScrollOnUpdate && terminal().viewport().scrolled()
        && terminal().inputHandler().mode() == ViMode::Insert)
        terminal().viewport().scrollToBottom();

    if (terminal().hasInput())
        display_->post(bind(&TerminalSession::flushInput, this));

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

void TerminalSession::requestCaptureBuffer(LineCount lines, bool logical)
{
    if (!display_)
        return;

    display_->post([this, lines, logical]() {
        if (display_->requestPermission(profile_.permissions.captureBuffer, "capture screen buffer"))
        {
            terminal_.primaryScreen().captureBuffer(lines, logical);
            DisplayLog()("requestCaptureBuffer: Finished. Waking up I/O thread.");
            flushInput();
        }
    });
}

terminal::FontDef TerminalSession::getFontDef()
{
    return display_->getFontDef();
}

void TerminalSession::setFontDef(terminal::FontDef const& _fontDef)
{
    display_->post([this, spec = terminal::FontDef(_fontDef)]() {
        if (!display_->requestPermission(profile_.permissions.changeFont, "changing font"))
            return;

        auto const& currentFonts = profile_.fonts;
        terminal::rasterizer::FontDescriptions newFonts = currentFonts;

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
    });
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
    if (!display_)
        return;

    display_->notify(_title, _content);
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

void TerminalSession::requestWindowResize(Width _width, Height _height)
{
    if (!display_)
        return;

    SessionLog()("Application request to resize window: {}x{} px", _width, _height);
    display_->post([this, _width, _height]() { display_->resizeWindow(_width, _height); });
}

void TerminalSession::setWindowTitle(string_view _title)
{
    if (!display_)
        return;

    display_->post([this, terminalTitle = string(_title)]() { display_->setWindowTitle(terminalTitle); });
}

void TerminalSession::setTerminalProfile(string const& _configProfileName)
{
    if (!display_)
        return;

    display_->post([this, name = string(_configProfileName)]() { activateProfile(name); });
}

void TerminalSession::discardImage(terminal::Image const& _image)
{
    if (!display_)
        return;

    display_->discardImage(_image);
}

void TerminalSession::inputModeChanged(terminal::ViMode mode)
{
    using terminal::ViMode;
    switch (mode)
    {
        case ViMode::Insert: configureCursor(profile_.inputModes.insert.cursor); break;
        case ViMode::Normal: configureCursor(profile_.inputModes.normal.cursor); break;
        case ViMode::Visual:
        case ViMode::VisualLine:
        case ViMode::VisualBlock: configureCursor(profile_.inputModes.visual.cursor); break;
    }
}

// }}}
// {{{ Input Events
void TerminalSession::sendKeyPressEvent(Key _key, Modifier _modifier, Timestamp _now)
{
    InputLog()("key press: {} {}", _modifier, _key);

    if (terminatedAndWaitingForKeyPress_)
    {
        display_->closeDisplay();
        return;
    }

    display_->setMouseCursorShape(MouseCursorShape::Hidden);

    if (auto const* actions =
            config::apply(config_.inputMappings.keyMappings, _key, _modifier, matchModeFlags()))
        executeAllActions(*actions);
    else
        terminal().sendKeyPressEvent(_key, _modifier, _now);
}

void TerminalSession::sendCharPressEvent(char32_t _value, Modifier _modifier, Timestamp _now)
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

    display_->setMouseCursorShape(MouseCursorShape::Hidden);

    if (auto const* actions =
            config::apply(config_.inputMappings.charMappings, _value, _modifier, matchModeFlags()))
        executeAllActions(*actions);
    else
        terminal().sendCharPressEvent(_value, _modifier, _now); // TODO: get rid of Event{} struct here, too!
}

void TerminalSession::sendMousePressEvent(Modifier _modifier,
                                          MouseButton _button,
                                          PixelCoordinate _pixelPosition,
                                          Timestamp _now)
{
    // InputLog()("sendMousePressEvent: {} {} at {}", _button, _modifier, currentMousePosition_);
    bool mouseGrabbed = terminal().isMouseGrabbedByApp();

    auto actionHandled = false;
    if (!mouseGrabbed)
    {
        if (auto const* actions =
                config::apply(config_.inputMappings.mouseMappings, _button, _modifier, matchModeFlags()))
        {
            if (executeAllActions(*actions))
            {
                actionHandled = true;
            }
        }
    }

    // clang-format off
    auto const selectionHandled = !actionHandled
                                  && !mouseGrabbed
                                  && _button == MouseButton::Left
                                  && terminal_.handleMouseSelection(_modifier, _now);
    // clang-format on

    // First try to pass the mouse event to the application, as it might have requested that.
    auto uiHandledHint = actionHandled || selectionHandled;
    auto const terminalHandled =
        terminal().sendMousePressEvent(_modifier, _button, _pixelPosition, uiHandledHint, _now);

    if (uiHandledHint || terminalHandled)
        scheduleRedraw();
}

void TerminalSession::sendMouseMoveEvent(terminal::Modifier _modifier,
                                         terminal::CellLocation _pos,
                                         terminal::PixelCoordinate _pixelPosition,
                                         Timestamp _now)
{
    // NB: This translation depends on the display's margin, so maybe
    //     the display should provide the translation?

    if (!(_pos < terminal().pageSize()))
        return;

    auto const uiHandledHint = false;
    auto const handled = terminal().sendMouseMoveEvent(_modifier, _pos, _pixelPosition, uiHandledHint, _now);

    if (_pos == currentMousePosition_)
        return;

    bool const mouseHoveringHyperlink = terminal().isMouseHoveringHyperlink();
    currentMousePosition_ = _pos;
    if (mouseHoveringHyperlink)
        display_->setMouseCursorShape(MouseCursorShape::PointingHand);
    else
        setDefaultCursor();

    // TODO: enter this if only if: `&& only if selection has changed!`
    if (mouseHoveringHyperlink || handled || terminal().isSelectionInProgress())
    {
        terminal().breakLoopAndRefreshRenderBuffer();
        scheduleRedraw();
    }
}

void TerminalSession::sendMouseReleaseEvent(Modifier _modifier,
                                            MouseButton _button,
                                            PixelCoordinate _pixelPosition,
                                            Timestamp _now)
{
    auto const uiHandledHint = false;
    terminal().sendMouseReleaseEvent(_modifier, _button, _pixelPosition, uiHandledHint, _now);
    scheduleRedraw();
}

void TerminalSession::sendFocusInEvent()
{
    // as per Qt-documentation, some platform implementations reset the cursor when leaving the
    // window, so we have to re-apply our desired cursor in focusInEvent().
    setDefaultCursor();

    terminal().sendFocusInEvent();

    display_->setBlurBehind(profile().backgroundBlur);
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

void TerminalSession::playSound(terminal::Sequence::Parameters const& params_)
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
    SessionLog()("Action is called");
    switch (copySelection.format)
    {
        case actions::CopyFormat::Text:
            // Copy the selection in pure text, plus whitespaces and newline.
            SessionLog()("Text");
            copyToClipboard(terminal().extractSelectionText());
            break;
        case actions::CopyFormat::HTML:
            SessionLog()("HTML");
            copyToClipboard(terminal().extractSelectionTextAsHtml());
            // I select some text in contour terminal and then i press ctrl H in keyboard (to override format in CopySelection)
            // The following should happen:
            // Loop through the selected cells of a grid. (Corresponding classes RenderCell with RenderAttributes???)
            // Map the colour,font, from RenderAttributes or enum class CellFlags and construct the html

            // I found the following in the kde terminal
            // https://github.com/KDE/konsole/blob/master/src/decoders/HTMLDecoder.cpp
            // https://github.com/KDE/konsole/blob/master/src/autotests/TerminalCharacterDecoderTest.cpp (see function TerminalCharacterDecoderTest::testHTMLDecoder_data() )
            //
            // monospace bold(enum class CellFlags ) 000000 ffffff are parameters (in our case properties are coming from the cell)
            // <span style="font-family:monospace"><span style="font-weight:bold;color:#000000;background-color:#ffffff;">hello</span><br></span>
            // I checked how the final output in the terminal would look like using https://html.onlineviewer.net/ the above will write hello in bold with white brackgournd
            // TODO: This requires walking through each selected cell and construct HTML+CSS for it.
            break;
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
    display_->setBackgroundOpacity(profile_.backgroundOpacity);
    return true;
}

bool TerminalSession::operator()(actions::FocusNextSearchMatch)
{
    if (terminal_.state().viCommands.jumpToNextMatch(1))
        terminal_.viewport().makeVisible(terminal_.state().viCommands.cursorPosition.line);
    // TODO why didn't the makeVisible() call from inside jumpToNextMatch not work?
    return true;
}

bool TerminalSession::operator()(actions::FocusPreviousSearchMatch)
{
    if (terminal_.state().viCommands.jumpToPreviousMatch(1))
        terminal_.viewport().makeVisible(terminal_.state().viCommands.cursorPosition.line);
    // TODO why didn't the makeVisible() call from inside jumpToPreviousMatch not work?
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
    if (static_cast<uint8_t>(profile_.backgroundOpacity) >= 255)
        return true;

    ++profile_.backgroundOpacity;
    display_->setBackgroundOpacity(profile_.backgroundOpacity);
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
    terminal().viewport().scrollDown(profile_.historyScrollMultiplier);
    return true;
}

bool TerminalSession::operator()(actions::ScrollMarkDown)
{
    terminal().viewport().scrollMarkDown();
    return true;
}

bool TerminalSession::operator()(actions::ScrollMarkUp)
{
    terminal().viewport().scrollMarkUp();
    return true;
}

bool TerminalSession::operator()(actions::ScrollOneDown)
{
    terminal().viewport().scrollDown(LineCount(1));
    return true;
}

bool TerminalSession::operator()(actions::ScrollOneUp)
{
    terminal().viewport().scrollUp(LineCount(1));
    return true;
}

bool TerminalSession::operator()(actions::ScrollPageDown)
{
    auto const stepSize = terminal().pageSize().lines / LineCount(2);
    terminal().viewport().scrollDown(stepSize);
    return true;
}

bool TerminalSession::operator()(actions::ScrollPageUp)
{
    auto const stepSize = terminal().pageSize().lines / LineCount(2);
    terminal().viewport().scrollUp(stepSize);
    return true;
}

bool TerminalSession::operator()(actions::ScrollToBottom)
{
    terminal().viewport().scrollToBottom();
    return true;
}

bool TerminalSession::operator()(actions::ScrollToTop)
{
    terminal().viewport().scrollToTop();
    return true;
}

bool TerminalSession::operator()(actions::ScrollUp)
{
    terminal().viewport().scrollUp(profile_.historyScrollMultiplier);
    return true;
}

bool TerminalSession::operator()(actions::SearchReverse)
{
    terminal_.inputHandler().startSearchExternally();

    return true;
}

bool TerminalSession::operator()(actions::SendChars const& _event)
{
    auto const now = steady_clock::now();

    for (auto const ch: _event.chars)
    {
        terminal().sendCharPressEvent(static_cast<char32_t>(ch), terminal::Modifier::None, now);
    }
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
    if (terminal().state().statusDisplayType != StatusDisplayType::Indicator)
        terminal().setStatusDisplay(StatusDisplayType::Indicator);
    else
        terminal().setStatusDisplay(StatusDisplayType::None);

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
    terminal_.setExecutionMode(ExecutionMode::BreakAtEmptyQueue);
    return true;
}

bool TerminalSession::operator()(actions::TraceEnter)
{
    terminal_.setExecutionMode(ExecutionMode::Waiting);
    return true;
}

bool TerminalSession::operator()(actions::TraceLeave)
{
    terminal_.setExecutionMode(ExecutionMode::Normal);
    return true;
}

bool TerminalSession::operator()(actions::TraceStep)
{
    terminal_.setExecutionMode(ExecutionMode::SingleStep);
    return true;
}
// }}}

bool TerminalSession::operator()(actions::ViNormalMode)
{
    terminal().inputHandler().setMode(ViMode::Normal);
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
    using Type = terminal::ScreenType;
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
        ::contour::spawnNewTerminal(
            app_.programPath(), config_.backingFilePath.generic_string(), _profileName, wd);
    }
    else
    {
        auto config = config_;
        config.profile(profileName_)->shell.workingDirectory = FileSystem::path(wd);
        app_.newWindow(config);
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
    terminal_.setMode(terminal::DECMode::NoSixelScrolling, !config_.sixelScrolling);
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
    terminal_.viewport().setScrollOff(profile_.modalCursorScrollOff);
}

void TerminalSession::configureCursor(config::CursorConfig const& cursorConfig)
{
    terminal_.setCursorBlinkingInterval(cursorConfig.cursorBlinkInterval);
    terminal_.setCursorDisplay(cursorConfig.cursorDisplay);
    terminal_.setCursorShape(cursorConfig.cursorShape);
}

void TerminalSession::configureDisplay()
{
    if (!display_)
        return;

    SessionLog()("Configuring display.");
    display_->setBlurBehind(profile_.backgroundBlur);

    display_->setBackgroundImage(profile_.colors.backgroundImage);

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

    display_->setWindowTitle(terminal_.windowTitle());

    display_->logDisplayTopInfo();
}

uint8_t TerminalSession::matchModeFlags() const
{
    uint8_t flags = 0;

    if (terminal_.isAlternateScreen())
        flags |= static_cast<uint8_t>(MatchModes::Flag::AlternateScreen);

    if (terminal_.applicationCursorKeys())
        flags |= static_cast<uint8_t>(MatchModes::Flag::AppCursor);

    if (terminal_.applicationKeypad())
        flags |= static_cast<uint8_t>(MatchModes::Flag::AppKeypad);

    if (terminal_.selectionAvailable())
        flags |= static_cast<uint8_t>(MatchModes::Flag::Select);

    if (terminal_.inputHandler().mode() == ViMode::Insert)
        flags |= static_cast<uint8_t>(MatchModes::Flag::Insert);

    if (!terminal_.state().searchMode.pattern.empty())
        flags |= static_cast<uint8_t>(MatchModes::Flag::Search);

    if (terminal_.executionMode() != ExecutionMode::Normal)
        flags |= static_cast<uint8_t>(MatchModes::Flag::Trace);

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

void TerminalSession::followHyperlink(terminal::HyperlinkInfo const& _hyperlink)
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

bool TerminalSession::requestPermission(config::Permission _allowedByConfig, string_view _topicText)
{
    return display_->requestPermission(_allowedByConfig, _topicText);
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

} // namespace contour
