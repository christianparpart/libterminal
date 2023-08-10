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
#include <vtbackend/ViInputHandler.h>
#include <vtbackend/logging.h>

#include <crispy/assert.h>
#include <crispy/utils.h>

#include <variant>

#include <libunicode/convert.h>

using std::nullopt;
using std::optional;
using std::pair;
using std::vector;
using namespace std::string_view_literals;

namespace terminal
{

// Possible future improvements (aka. nice TODO):
//
// [ ] motion f{char}
// [ ] motion t{char}
// [ ] motion %
// [ ] motion to jump marks up/down
// [ ] add timer to disable selection (needs timer API inside of libterminal)
// [ ] show cursor if it was hidden and restore it when going back to insert mode
// [ ] remember initial cursor shae and restore it when going back to insert mode

namespace
{
    struct InputMatch
    {
        // ViMode mode; // TODO: ideally we also would like to match on input Mode
        modifier modifier;
        char32_t ch;

        [[nodiscard]] constexpr uint32_t code() const noexcept
        {
            return uint32_t(ch << 5) | uint32_t(modifier.value() & 0b1'1111);
        }

        constexpr operator uint32_t() const noexcept
        {
            return uint32_t(ch << 5) | uint32_t(modifier.value() & 0b1'1111);
        }
    };

    constexpr InputMatch operator"" _key(char ch)
    {
        return InputMatch { modifier::None, static_cast<char32_t>(ch) };
    }

    constexpr InputMatch operator|(modifier::key mod, char ch) noexcept
    {
        return InputMatch { modifier { mod }, (char32_t) ch };
    }
} // namespace

ViInputHandler::ViInputHandler(Executor& theExecutor, vi_mode initialMode):
    _viMode { initialMode }, _executor { theExecutor }
{
    registerAllCommands();
}

void ViInputHandler::registerAllCommands()
{
    auto constexpr scopeMappings =
        std::array<std::pair<char, TextObjectScope>, 2> { { std::pair { 'i', TextObjectScope::Inner },
                                                            std::pair { 'a', TextObjectScope::A } } };

    auto constexpr motionMappings = std::array<std::pair<std::string_view, ViMotion>, 43> { {
        // clang-format off
        { "$", ViMotion::LineEnd },
        { "%", ViMotion::ParenthesisMatching },
        { "0", ViMotion::LineBegin },
        { "<BS>", ViMotion::CharLeft },
        { "<NL>", ViMotion::LineDown },
        { "<Down>", ViMotion::LineDown },
        { "<End>", ViMotion::LineEnd },
        { "<Home>", ViMotion::LineBegin },
        { "<Left>", ViMotion::CharLeft },
        { "<PageDown>", ViMotion::PageDown },
        { "<PageUp>", ViMotion::PageUp },
        { "<Right>", ViMotion::CharRight },
        { "<Space>", ViMotion::CharRight },
        { "<Up>", ViMotion::LineUp },
        { "B", ViMotion::BigWordBackward },
        { "C-D", ViMotion::PageDown },
        { "C-U", ViMotion::PageUp },
        { "E", ViMotion::BigWordEndForward },
        { "G", ViMotion::FileEnd },
        { "H", ViMotion::PageTop },
        { "L", ViMotion::PageBottom },
        { "M", ViMotion::LinesCenter },
        { "N", ViMotion::SearchResultBackward },
        { "W", ViMotion::BigWordForward },
        { "[[", ViMotion::GlobalCurlyOpenUp },
        { "[]", ViMotion::GlobalCurlyCloseUp },
        { "[m", ViMotion::LineMarkUp },
        { "][", ViMotion::GlobalCurlyCloseDown },
        { "]]", ViMotion::GlobalCurlyOpenDown },
        { "]m", ViMotion::LineMarkDown },
        { "^", ViMotion::LineTextBegin },
        { "b", ViMotion::WordBackward },
        { "e", ViMotion::WordEndForward },
        { "gg", ViMotion::FileBegin },
        { "h", ViMotion::CharLeft },
        { "j", ViMotion::LineDown },
        { "k", ViMotion::LineUp },
        { "l", ViMotion::CharRight },
        { "n", ViMotion::SearchResultForward },
        { "w", ViMotion::WordForward },
        { "{", ViMotion::ParagraphBackward },
        { "|", ViMotion::ScreenColumn },
        { "}", ViMotion::ParagraphForward },
        // clang-format on
    } };

    auto constexpr textObjectMappings = std::array<std::pair<char, TextObject>, 15> { {
        { '"', TextObject::DoubleQuotes },
        { 'm', TextObject::LineMark },
        { '(', TextObject::RoundBrackets },
        { ')', TextObject::RoundBrackets },
        { '<', TextObject::AngleBrackets },
        { '>', TextObject::AngleBrackets },
        { 'W', TextObject::BigWord },
        { '[', TextObject::SquareBrackets },
        { ']', TextObject::SquareBrackets },
        { '\'', TextObject::SingleQuotes },
        { '`', TextObject::BackQuotes },
        { 'p', TextObject::Paragraph },
        { 'w', TextObject::Word },
        { '{', TextObject::CurlyBrackets },
        { '}', TextObject::CurlyBrackets },
    } };

    // normal mode and visual mode
    // clang-format off
    for (auto const modeSelect: { ModeSelect::Normal, ModeSelect::Visual })
    {
        for (auto const& [motionChar, motion]: motionMappings)
            registerCommand(
                modeSelect, motionChar, [this, motion = motion]() { _executor.moveCursor(motion, count()); });

        registerCommand(modeSelect, "J", [this]() { _executor.scrollViewport(scroll_offset(-1)); _executor.moveCursor(ViMotion::LineDown, 1);});
        registerCommand(modeSelect, "K", [this]() { _executor.scrollViewport(scroll_offset(+1)); _executor.moveCursor(ViMotion::LineUp, 1);});

        registerCommand(modeSelect, "t.", [this]() { _executor.moveCursor(ViMotion::TillBeforeCharRight, count(), _lastChar); });
        registerCommand(modeSelect, "T.", [this]() { _executor.moveCursor(ViMotion::TillAfterCharLeft, count(), _lastChar); });
        registerCommand(modeSelect, "f.", [this]() { _executor.moveCursor(ViMotion::ToCharRight, count(), _lastChar); });
        registerCommand(modeSelect, "F.", [this]() { _executor.moveCursor(ViMotion::ToCharLeft, count(), _lastChar); });
        registerCommand(modeSelect, ";", [this]() { _executor.moveCursor(ViMotion::RepeatCharMove, count()); });
        registerCommand(modeSelect, ",", [this]() { _executor.moveCursor(ViMotion::RepeatCharMoveReverse, count()); });
    }

    registerCommand(ModeSelect::Normal, "a", [this]() { setMode(vi_mode::Insert); });
    registerCommand(ModeSelect::Normal, "i", [this]() { setMode(vi_mode::Insert); });
    registerCommand(ModeSelect::Normal, "<Insert>", [this]() { setMode(vi_mode::Insert); });
    registerCommand(ModeSelect::Normal, "v", [this]() { toggleMode(vi_mode::Visual); });
    registerCommand(ModeSelect::Normal, "V", [this]() { toggleMode(vi_mode::VisualLine); });
    registerCommand(ModeSelect::Normal, "C-V", [this]() { toggleMode(vi_mode::VisualBlock); });
    registerCommand(ModeSelect::Normal, "/", [this]() { startSearch(); });
    registerCommand(ModeSelect::Normal, "#", [this]() { _executor.reverseSearchCurrentWord(); });
    registerCommand(ModeSelect::Normal, "mm", [this]() { _executor.toggleLineMark(); });
    registerCommand(ModeSelect::Normal, "*", [this]() { _executor.searchCurrentWord(); });
    registerCommand(ModeSelect::Normal, "p", [this]() { _executor.paste(count(), false); });
    registerCommand(ModeSelect::Normal, "P", [this]() { _executor.paste(count(), true); });

    registerCommand(ModeSelect::Normal, "Y", [this]() { _executor.execute(ViOperator::Yank, ViMotion::FullLine, count()); });
    registerCommand(ModeSelect::Normal, "yy", [this]() { _executor.execute(ViOperator::Yank, ViMotion::FullLine, count()); });
    registerCommand(ModeSelect::Normal, "yb", [this]() { _executor.execute(ViOperator::Yank, ViMotion::WordBackward, count()); });
    registerCommand(ModeSelect::Normal, "ye", [this]() { _executor.execute(ViOperator::Yank, ViMotion::WordEndForward, count()); });
    registerCommand(ModeSelect::Normal, "yw", [this]() { _executor.execute(ViOperator::Yank, ViMotion::WordForward, count()); });
    registerCommand(ModeSelect::Normal, "yB", [this]() { _executor.execute(ViOperator::Yank, ViMotion::BigWordBackward, count()); });
    registerCommand(ModeSelect::Normal, "yE", [this]() { _executor.execute(ViOperator::Yank, ViMotion::BigWordEndForward, count()); });
    registerCommand(ModeSelect::Normal, "yW", [this]() { _executor.execute(ViOperator::Yank, ViMotion::BigWordForward, count()); });
    registerCommand(ModeSelect::Normal, "yt.", [this]() { _executor.execute(ViOperator::Yank, ViMotion::TillBeforeCharRight, count(), _lastChar); });
    registerCommand(ModeSelect::Normal, "yT.", [this]() { _executor.execute(ViOperator::Yank, ViMotion::TillAfterCharLeft, count(), _lastChar); });
    registerCommand(ModeSelect::Normal, "yf.", [this]() { _executor.execute(ViOperator::Yank, ViMotion::ToCharRight, count(), _lastChar); });
    registerCommand(ModeSelect::Normal, "yF.", [this]() { _executor.execute(ViOperator::Yank, ViMotion::ToCharLeft, count(), _lastChar); });
    // clang-format on

    for (auto const& [scopeChar, scope]: scopeMappings)
        for (auto const& [objectChar, obj]: textObjectMappings)
            registerCommand(ModeSelect::Normal,
                            fmt::format("y{}{}", scopeChar, objectChar),
                            [this, scope = scope, obj = obj]() { _executor.yank(scope, obj); });

    // visual mode
    registerCommand(ModeSelect::Visual, "/", [this]() { startSearch(); });
    registerCommand(ModeSelect::Visual, "y", [this]() {
        _executor.execute(ViOperator::Yank, ViMotion::Selection, count());
    });
    registerCommand(ModeSelect::Visual, "v", [this]() { toggleMode(vi_mode::Normal); });
    registerCommand(ModeSelect::Visual, "V", [this]() { toggleMode(vi_mode::VisualLine); });
    registerCommand(ModeSelect::Visual, "C-V", [this]() { toggleMode(vi_mode::VisualBlock); });
    registerCommand(ModeSelect::Visual, "<ESC>", [this]() { setMode(vi_mode::Normal); });
    for (auto const& [scopeChar, scope]: scopeMappings)
        for (auto const& [objectChar, obj]: textObjectMappings)
            registerCommand(ModeSelect::Visual,
                            fmt::format("{}{}", scopeChar, objectChar),
                            [this, scope = scope, obj = obj]() { _executor.select(scope, obj); });
}

void ViInputHandler::registerCommand(ModeSelect modes,
                                     std::vector<std::string_view> const& commands,
                                     CommandHandler const& handler)
{
    for (auto const& command: commands)
        registerCommand(modes, command, handler);
}

void ViInputHandler::registerCommand(ModeSelect modes, std::string_view command, CommandHandler handler)
{
    Require(!!handler);

    auto commandStr = crispy::replace(std::string(command.data(), command.size()), "<Space>", " ");

    switch (modes)
    {
        case ModeSelect::Normal: {
            InputLog()("Registering normal mode command: {}", command);
            Require(!_normalMode.contains(commandStr));
            _normalMode.insert(commandStr, std::move(handler));
            break;
        }
        case ModeSelect::Visual: {
            InputLog()("Registering visual mode command: {}", command);
            Require(!_visualMode.contains(commandStr));
            _visualMode.insert(commandStr, std::move(handler));
            break;
        }
        default: crispy::unreachable();
    }
}

void ViInputHandler::appendModifierToPendingInput(modifier modifier)
{
    if (modifier.meta())
        _pendingInput += "M-";
    if (modifier.alt())
        _pendingInput += "A-";
    if (modifier.shift())
        _pendingInput += "S-";
    if (modifier.control())
        _pendingInput += "C-";
}

bool ViInputHandler::handlePendingInput()
{
    Require(!_pendingInput.empty());

    auto constexpr TrieMapAllowWildcardDot = true;

    CommandHandlerMap const& mapping = isVisualMode() ? _visualMode : _normalMode;
    auto const mappingResult = mapping.search(_pendingInput, TrieMapAllowWildcardDot);
    if (std::holds_alternative<crispy::ExactMatch<CommandHandler>>(mappingResult))
    {
        InputLog()("Executing handler for: {}{}", _count ? fmt::format("{} ", _count) : "", _pendingInput);
        _lastChar =
            unicode::convert_to<char32_t>(std::string_view(_pendingInput.data(), _pendingInput.size()))
                .back();
        std::get<crispy::ExactMatch<CommandHandler>>(mappingResult).value();
        clearPendingInput();
    }
    else if (std::holds_alternative<crispy::NoMatch>(mappingResult))
    {
        InputLog()("Invalid command: {}", _pendingInput);
        clearPendingInput();
    }
    else
    {
        InputLog()("Incomplete input: {}", _pendingInput);
    }

    return true;
}

void ViInputHandler::clearPendingInput()
{
    InputLog()("Resetting pending input: {}", _pendingInput);
    _count = 0;
    _pendingInput.clear();
}

void ViInputHandler::setMode(vi_mode theMode)
{
    if (_viMode == theMode)
        return;

    _viMode = theMode;
    clearPendingInput();

    _executor.modeChanged(theMode);
}

bool ViInputHandler::sendKeyPressEvent(key k, modifier modifier)
{
    if (_searchEditMode != SearchEditMode::Disabled)
    {
        // Do we want to do anything in here?
        // TODO: support cursor movements.
        errorlog()("ViInputHandler: Ignoring key input {}+{}.", modifier, k);
        return true;
    }

    // clang-format off
    switch (_viMode)
    {
        case vi_mode::Insert:
            return false;
        case vi_mode::Normal:
        case vi_mode::Visual:
        case vi_mode::VisualLine:
        case vi_mode::VisualBlock:
            break;
    }
    // clang-format on

    if (modifier.any())
        return true;

    auto const keyMappings = std::array<std::pair<key, std::string_view>, 10> { {
        { key::DownArrow, "<Down>" },
        { key::LeftArrow, "<Left>" },
        { key::RightArrow, "<Right>" },
        { key::UpArrow, "<Up>" },
        { key::Insert, "<Insert>" },
        { key::Delete, "<Delete>" },
        { key::Home, "<Home>" },
        { key::End, "<End>" },
        { key::PageUp, "<PageUp>" },
        { key::PageDown, "<PageDown>" },
    } };

    for (auto const& [mappedKey, mappedText]: keyMappings)
        if (k == mappedKey)
        {
            _pendingInput += mappedText;
            break;
        }

    return handlePendingInput();
}

void ViInputHandler::startSearchExternally()
{
    _searchTerm.clear();
    _executor.searchStart();

    if (_viMode != vi_mode::Insert)
        _searchEditMode = SearchEditMode::Enabled;
    else
    {
        _searchEditMode = SearchEditMode::ExternallyEnabled;
        setMode(vi_mode::Normal);
        // ^^^ So that we can see the statusline (which contains the search edit field),
        // AND it's weird to be in insert mode while typing in the search term anyways.
    }
}

bool ViInputHandler::handleSearchEditor(char32_t ch, modifier modifier)
{
    assert(_searchEditMode != SearchEditMode::Disabled);

    switch (InputMatch { modifier, ch })
    {
        case '\x1B'_key:
            _searchTerm.clear();
            if (_searchEditMode == SearchEditMode::ExternallyEnabled)
                setMode(vi_mode::Insert);
            _searchEditMode = SearchEditMode::Disabled;
            _executor.searchCancel();
            break;
        case '\x0D'_key:
            if (_searchEditMode == SearchEditMode::ExternallyEnabled)
                setMode(vi_mode::Insert);
            _searchEditMode = SearchEditMode::Disabled;
            _executor.searchDone();
            break;
        case '\x08'_key:
        case '\x7F'_key:
            if (!_searchTerm.empty())
                _searchTerm.resize(_searchTerm.size() - 1);
            _executor.updateSearchTerm(_searchTerm);
            break;
        case modifier::Control | 'L':
        case modifier::Control | 'U':
            _searchTerm.clear();
            _executor.updateSearchTerm(_searchTerm);
            break;
        case modifier::Control | 'A': // TODO: move cursor to BOL
        case modifier::Control | 'E': // TODO: move cursor to EOL
        default:
            if (ch >= 0x20 && modifier.without(modifier::Shift).none())
            {
                _searchTerm += ch;
                _executor.updateSearchTerm(_searchTerm);
            }
            else
                errorlog()("ViInputHandler: Receiving control code {}+0x{:02X} in search mode. Ignoring.",
                           modifier,
                           (unsigned) ch);
    }

    return true;
}

bool ViInputHandler::sendCharPressEvent(char32_t ch, modifier modifier)
{
    if (_searchEditMode != SearchEditMode::Disabled)
        return handleSearchEditor(ch, modifier);

    if (_viMode == vi_mode::Insert)
        return false;

    if (ch == 033 && modifier.none())
    {
        clearPendingInput();
        setMode(vi_mode::Normal);
        return true;
    }

    if (parseCount(ch, modifier))
        return true;

    appendModifierToPendingInput(ch > 0x20 ? modifier.without(modifier::Shift) : modifier);

    if (ch == '\033')
        _pendingInput += "<ESC>";
    else if (ch == '\b')
        _pendingInput += "<BS>";
    else if (ch == '\n' || ch == '\r')
        _pendingInput += "<NL>";
    else
        _pendingInput += unicode::convert_to<char>(ch);

    if (handlePendingInput())
        return true;

    return false;
}

bool ViInputHandler::parseCount(char32_t ch, modifier modifier)
{
    if (!modifier.none())
        return false;

    switch (ch)
    {
        case '0':
            if (!_count)
                break;
            [[fallthrough]];
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            //.
            _count = _count * 10 + (ch - '0');
            return true;
    }
    return false;
}

void ViInputHandler::startSearch()
{
    _searchEditMode = SearchEditMode::Enabled;
    _executor.searchStart();
}

void ViInputHandler::toggleMode(vi_mode newMode)
{
    setMode(newMode != _viMode ? newMode : vi_mode::Normal);
}

} // namespace terminal
