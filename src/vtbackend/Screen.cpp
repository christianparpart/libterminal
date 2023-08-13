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
#include <vtbackend/ControlCode.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/Screen.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/VTType.h>
#include <vtbackend/VTWriter.h>
#include <vtbackend/logging.h>

#include <crispy/App.h>
#include <crispy/Comparison.h>
#include <crispy/algorithm.h>
#include <crispy/base64.h>
#include <crispy/escape.h>
#include <crispy/size.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <range/v3/view/iota.hpp>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>

#include "vtbackend/Color.h"
#include "vtbackend/Line.h"
#include "vtbackend/SixelParser.h"
#include "vtbackend/primitives.h"
#include <libunicode/convert.h>
#include <libunicode/emoji_segmenter.h>
#include <libunicode/grapheme_segmenter.h>
#include <libunicode/word_segmenter.h>

using namespace std::string_view_literals;

using crispy::escape;
using crispy::for_each;
using crispy::times;
using crispy::toHexString;

using gsl::span;

using std::accumulate;
using std::array;
using std::clamp;
using std::endl;
using std::fill;
using std::function;
using std::get;
using std::holds_alternative;
using std::make_shared;
using std::make_unique;
using std::max;
using std::min;
using std::monostate;
using std::move;
using std::next;
using std::nullopt;
using std::optional;
using std::ostringstream;
using std::pair;
using std::prev;
using std::ref;
using std::rotate;
using std::shared_ptr;
using std::string;
using std::string_view;
using std::tuple;
using std::unique_ptr;
using std::vector;

namespace terminal
{

auto constexpr inline TabWidth = ColumnCount(8);

auto const inline VTCaptureBufferLog = logstore::Category("vt.ext.capturebuffer",
                                                          "Capture Buffer debug logging.",
                                                          logstore::Category::State::Disabled,
                                                          logstore::Category::Visibility::Hidden);

namespace // {{{ helper
{
    std::string vtSequenceParameterString(graphics_attributes const& sgr)
    {
        std::string output;
        // TODO: See if we can use VTWriter here instead (might need a little refactor).
        // std::stringstream os;
        // auto vtWriter = VTWriter(os);
        // vtWriter.setForegroundColor(sgr.foregroundColor);
        // vtWriter.setBackgroundColor(sgr.backgroundColor);

        auto const sgrSep = [&]() {
            if (!output.empty())
                output += ';';
        };
        auto const sgrAdd = [&](unsigned value) {
            sgrSep();
            output += std::to_string(value);
        };
        auto const sgrAddStr = [&](string_view value) {
            sgrSep();
            output += value;
        };
        auto const sgrAddSub = [&](unsigned value) {
            output += std::to_string(value);
        };

        if (isIndexedColor(sgr.foregroundColor))
        {
            auto const colorValue = getIndexedColor(sgr.foregroundColor);
            if (static_cast<unsigned>(colorValue) < 8)
                sgrAdd(30 + static_cast<unsigned>(colorValue));
            else
            {
                sgrAdd(38);
                sgrAddSub(5);
                sgrAddSub(static_cast<unsigned>(colorValue));
            }
        }
        else if (isDefaultColor(sgr.foregroundColor))
            sgrAdd(39);
        else if (isBrightColor(sgr.foregroundColor))
            sgrAdd(90 + static_cast<unsigned>(getBrightColor(sgr.foregroundColor)));
        else if (isRGBColor(sgr.foregroundColor))
        {
            auto const rgb = getRGBColor(sgr.foregroundColor);
            sgrAdd(38);
            sgrAddSub(2);
            sgrAddSub(static_cast<unsigned>(rgb.red));
            sgrAddSub(static_cast<unsigned>(rgb.green));
            sgrAddSub(static_cast<unsigned>(rgb.blue));
        }

        if (isIndexedColor(sgr.backgroundColor))
        {
            auto const colorValue = getIndexedColor(sgr.backgroundColor);
            if (static_cast<unsigned>(colorValue) < 8)
                sgrAdd(40 + static_cast<unsigned>(colorValue));
            else
            {
                sgrAdd(48);
                sgrAddSub(5);
                sgrAddSub(static_cast<unsigned>(colorValue));
            }
        }
        else if (isDefaultColor(sgr.backgroundColor))
            sgrAdd(49);
        else if (isBrightColor(sgr.backgroundColor))
            sgrAdd(100 + getBrightColor(sgr.backgroundColor));
        else if (isRGBColor(sgr.backgroundColor))
        {
            auto const& rgb = getRGBColor(sgr.backgroundColor);
            sgrAdd(48);
            sgrAddSub(2);
            sgrAddSub(static_cast<unsigned>(rgb.red));
            sgrAddSub(static_cast<unsigned>(rgb.green));
            sgrAddSub(static_cast<unsigned>(rgb.blue));
        }

        if (isRGBColor(sgr.underlineColor))
        {
            auto const& rgb = getRGBColor(sgr.underlineColor);
            sgrAdd(58);
            sgrAddSub(2);
            sgrAddSub(static_cast<unsigned>(rgb.red));
            sgrAddSub(static_cast<unsigned>(rgb.green));
            sgrAddSub(static_cast<unsigned>(rgb.blue));
        }

        // TODO: sgr.styles;
        auto constexpr masks = array {
            pair { cell_flags::Bold, "1"sv },
            pair { cell_flags::Faint, "2"sv },
            pair { cell_flags::Italic, "3"sv },
            pair { cell_flags::Underline, "4"sv },
            pair { cell_flags::Blinking, "5"sv },
            pair { cell_flags::RapidBlinking, "6"sv },
            pair { cell_flags::Inverse, "7"sv },
            pair { cell_flags::Hidden, "8"sv },
            pair { cell_flags::CrossedOut, "9"sv },
            pair { cell_flags::DoublyUnderlined, "4:2"sv },
            pair { cell_flags::CurlyUnderlined, "4:3"sv },
            pair { cell_flags::DottedUnderline, "4:4"sv },
            pair { cell_flags::DashedUnderline, "4:5"sv },
            pair { cell_flags::Framed, "51"sv },
            // TODO(impl or completely remove): pair{cell_flags::Encircled, ""sv},
            pair { cell_flags::Overline, "53"sv },
        };

        for (auto const& mask: masks)
            if (sgr.flags & mask.first)
                sgrAddStr(mask.second);

        return output;
    }

    template <typename T, typename U>
    std::optional<crispy::boxed<T, U>> decr(std::optional<crispy::boxed<T, U>> v)
    {
        if (v.has_value())
            --*v;
        return v;
    }

    // optional<CharsetTable> getCharsetTableForCode(std::string const& intermediate)
    // {
    //     if (intermediate.size() != 1)
    //         return nullopt;
    //
    //     char const code = intermediate[0];
    //     switch (code)
    //     {
    //         case '(':
    //             return {CharsetTable::G0};
    //         case ')':
    //         case '-':
    //             return {CharsetTable::G1};
    //         case '*':
    //         case '.':
    //             return {CharsetTable::G2};
    //         case '+':
    //         case '/':
    //             return {CharsetTable::G3};
    //         default:
    //             return nullopt;
    //     }
    // }
} // namespace
// }}}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Screen<Cell>::Screen(Terminal& terminal,
                     PageSize pageSize,
                     bool reflowOnResize,
                     max_history_line_count maxHistoryLineCount):
    _terminal { terminal },
    _settings { terminal.settings() },
    _state { terminal.state() },
    _grid { pageSize, reflowOnResize, maxHistoryLineCount }
{
    updateCursorIterator();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
unsigned Screen<Cell>::numericCapability(capabilities::code cap) const
{
    using namespace capabilities::literals;

    switch (cap)
    {
        case "li"_tcap: return unbox<unsigned>(_settings.pageSize.lines);
        case "co"_tcap: return unbox<unsigned>(_settings.pageSize.columns);
        case "it"_tcap: return unbox<unsigned>(TabWidth);
        default: return static_database::numericCapability(cap);
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::verifyState() const
{
    margin& m = const_cast<Screen*>(this)->getMargin();
    Require(0 <= *m.hori.from && *m.hori.to < *pageSize().columns);
    Require(0 <= *m.vert.from && *m.vert.to < *pageSize().lines);
    Require(*_cursor.position.column < *_settings.pageSize.columns);
    Require(*_cursor.position.line < *_settings.pageSize.lines);

    // verify cursor positions
    [[maybe_unused]] auto const clampedCursorPos = clampToScreen(_cursor.position);
    if (_cursor.position != clampedCursorPos)
    {
        fail(fmt::format("Cursor {} does not match clamp to screen {}.", _cursor, clampedCursorPos));
        // FIXME: the above triggers on tmux vertical screen split (cursor.column off-by-one)
    }

    _grid.verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::fail(std::string const& message) const
{
    inspect(message, std::cerr);
    abort();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::hardReset()
{
    _grid.reset();
    _cursor = {};
    _lastCursorPosition = {};
    updateCursorIterator();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::applyPageSizeToMainDisplay(PageSize mainDisplayPageSize)
{
    auto cursorPosition = _cursor.position;

    // Ensure correct screen buffer size for the buffer we've just switched to.
    cursorPosition = _grid.resize(mainDisplayPageSize, cursorPosition, _cursor.wrapPending);
    cursorPosition = clampCoordinate(cursorPosition);

    auto const margin =
        terminal::margin { margin::vertical { {}, mainDisplayPageSize.lines.as<line_offset>() - 1 },
                           margin::horizontal { {}, mainDisplayPageSize.columns.as<column_offset>() - 1 } };

    _grid.getMargin() = margin;

    if (_cursor.position.column < boxed_cast<column_offset>(mainDisplayPageSize.columns))
        _cursor.wrapPending = false;

    // update (last-)cursor position
    _cursor.position = cursorPosition;
    _lastCursorPosition = cursorPosition;
    updateCursorIterator();

    // TODO: find out what to do with DECOM mode. Reset it to?
#if 0
    inspect("after resize", std::cout);
    fmt::print("applyPageSizeToCurrentBuffer: cursor pos before: {} after: {}\n", oldCursorPos, _cursor.position);
#endif

    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
string_view Screen<Cell>::tryEmplaceChars(string_view chars, size_t cellCount) noexcept
{
    if (!isFullHorizontalMargins())
        return chars;

    // In case the charset has been altered, no
    // optimization can be applied.
    // Unless we're storing the charset in the TrivialLineBuffer, too.
    // But for now that's too rare to be beneficial.
    if (!_cursor.charsets.isSelected(charset_id::USASCII))
        return chars;

    crlfIfWrapPending();

    auto const columnsAvailable = pageSize().columns.value - _cursor.position.column.value;
    assert(cellCount <= static_cast<size_t>(columnsAvailable));

    if (!_terminal.isModeEnabled(dec_mode::AutoWrap) && cellCount > static_cast<size_t>(columnsAvailable))
        // With AutoWrap on, we can only emplace if it fits the line.
        return chars;

    if (_cursor.position.column.value == 0)
    {
        if (currentLine().empty())
        {
            auto const numberOfBytesEmplaced = emplaceCharsIntoCurrentLine(chars, cellCount);
            _terminal.currentPtyBuffer()->advanceHotEndUntil(chars.data() + numberOfBytesEmplaced);
            chars.remove_prefix(numberOfBytesEmplaced);
            assert(chars.empty());
        }
        return chars;
    }

    if (isContiguousToCurrentLine(chars))
    {
        // We can append the chars to a pre-existing non-empty line.
        assert(static_cast<int>(cellCount) <= columnsAvailable);
        auto& lineBuffer = currentLine().trivialBuffer();
        lineBuffer.text.growBy(chars.size());
        lineBuffer.usedColumns += ColumnCount::cast_from(cellCount);
        advanceCursorAfterWrite(ColumnCount::cast_from(cellCount));
        _terminal.currentPtyBuffer()->advanceHotEndUntil(chars.data() + chars.size());
        chars.remove_prefix(chars.size());
        return chars;
    }

    return chars;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
size_t Screen<Cell>::emplaceCharsIntoCurrentLine(string_view chars, size_t cellCount) noexcept
{
    [[maybe_unused]] auto columnsAvailable = (getMargin().hori.to.value + 1) - _cursor.position.column.value;
    assert(cellCount <= static_cast<size_t>(columnsAvailable));

    line<Cell>& line = currentLine();
    if (line.isTrivialBuffer() && line.empty())
    {
        // Only use fastpath if the currently line hasn't been inflated already.
        // Because we might lose prior-written textual/SGR information otherwise.
        line.setBuffer(
            trivial_line_buffer { line.trivialBuffer().displayWidth,
                                  _cursor.graphicsRendition,
                                  line.trivialBuffer().fillAttributes,
                                  _cursor.hyperlink,
                                  ColumnCount::cast_from(cellCount),
                                  crispy::BufferFragment { _terminal.currentPtyBuffer(), chars } });
        advanceCursorAfterWrite(ColumnCount::cast_from(cellCount));
    }
    else
    {
        // Transforming chars input from UTF-8 to UTF-32 even though right now it should only
        // be containing US-ASCII, but soon it'll be any arbitrary textual Unicode codepoints.
        for (char const ch: chars)
        {
            _state.parser.printUtf8Byte(ch);
        }
    }
    return chars.size();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::advanceCursorAfterWrite(ColumnCount n) noexcept
{
    assert(_cursor.position.column.value + n.value <= getMargin().hori.to.value + 1);
    //  + 1 here because `to` is inclusive.

    if (_cursor.position.column.value + n.value < _settings.pageSize.columns.value)
        _cursor.position.column.value += n.value;
    else
    {
        _cursor.position.column.value += n.value - 1;
        _cursor.wrapPending = true;
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::writeText(string_view text, size_t cellCount)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTTraceSequenceLog)
        VTTraceSequenceLog()("text({} bytes, {} cells): \"{}\"", text.size(), cellCount, escape(text));
#endif
    assert(cellCount <= static_cast<size_t>(pageSize().columns.value - _cursor.position.column.value));

    text = tryEmplaceChars(text, cellCount);
    if (text.empty())
        return;

    // Making use of the optimized code path for the input characters did NOT work, so we need to first
    // convert UTF-8 to UTF-32 codepoints (reusing the logic in VT parser) and pass these codepoints
    // to the grapheme cluster processor.

    for (char const ch: text)
        _state.parser.printUtf8Byte(ch);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::writeTextFromExternal(std::string_view text)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTTraceSequenceLog)
        VTTraceSequenceLog()("external text: \"{}\"", text);
#endif

    for (char32_t const ch: unicode::convert_to<char32_t>(text))
        writeTextInternal(ch);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::crlfIfWrapPending()
{
    if (_cursor.wrapPending && _cursor.autoWrap) // && !_terminal.isModeEnabled(DECMode::TextReflow))
    {
        bool const lineWrappable = currentLine().wrappable();
        crlf();
        if (lineWrappable)
            currentLine().setFlag(line_flags::Wrappable | line_flags::Wrapped, true);
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::writeText(char32_t codepoint)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTTraceSequenceLog)
        VTTraceSequenceLog()("char: \'{}\'", unicode::convert_to<char>(codepoint));
#endif

    return writeTextInternal(codepoint);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::writeTextInternal(char32_t sourceCodepoint)
{
    crlfIfWrapPending();

    char32_t const codepoint = _cursor.charsets.map(sourceCodepoint);

    if (unicode::grapheme_segmenter::breakable(precedingGraphicCharacter(), codepoint))
    {
        writeCharToCurrentAndAdvance(codepoint);
    }
    else
    {
        auto const extendedWidth = usePreviousCell().appendCharacter(codepoint);
        if (extendedWidth > 0)
            clearAndAdvance(extendedWidth);
        _terminal.markCellDirty(_lastCursorPosition);
    }

    resetInstructionCounter();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::writeCharToCurrentAndAdvance(char32_t codepoint) noexcept
{
    line<Cell>& line = currentLine();

    Cell& cell = line.useCellAt(_cursor.position.column);

#if defined(LINE_AVOID_CELL_RESET)
    bool const consecutiveTextWrite = _state.sequencer.instructionCounter() == 1;
    if (!consecutiveTextWrite)
        cell.reset();
#endif

    cell.write(_cursor.graphicsRendition,
               codepoint,
               static_cast<uint8_t>(unicode::width(codepoint)),
               _cursor.hyperlink);

    _lastCursorPosition = _cursor.position;

    clearAndAdvance(cell.width());

    // TODO: maybe move selector API up? So we can make this call conditional,
    //       and only call it when something is selected?
    //       Alternatively we could add a boolean to make this callback
    //       conditional, something like: setReportDamage(bool);
    //       The latter is probably the easiest.
    _terminal.markCellDirty(_cursor.position);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::clearAndAdvance(int offset) noexcept
{
    if (offset == 0)
        return;

    bool const cursorInsideMargin =
        _terminal.isModeEnabled(dec_mode::LeftRightMargin) && isCursorInsideMargins();
    auto const cellsAvailable = cursorInsideMargin
                                    ? *(getMargin().hori.to - _cursor.position.column) - 1
                                    : *_settings.pageSize.columns - *_cursor.position.column - 1;
    auto const n = min(offset, cellsAvailable);

    if (n == offset)
    {
        _cursor.position.column++;
        auto& line = currentLine();
        for (int i = 1; i < n; ++i) // XXX It's not even clear if other TEs are doing that, too.
        {
            line.useCellAt(_cursor.position.column).reset(_cursor.graphicsRendition, _cursor.hyperlink);
            _cursor.position.column++;
        }
    }
    else if (_cursor.autoWrap)
    {
        _cursor.wrapPending = true;
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::string Screen<Cell>::screenshot(function<string(line_offset)> const& postLine) const
{
    auto result = std::stringstream {};
    auto writer = VTWriter(result);

    for (int const line: ::ranges::views::iota(0, *_settings.pageSize.lines))
    {
        writer.write(_grid.lineAt(line_offset(line)));
        if (postLine)
            writer.write(postLine(line_offset(line)));
        writer.crlf();
    }

    return result.str();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
optional<line_offset> Screen<Cell>::findMarkerUpwards(line_offset startLine) const
{
    // XXX startLine is an absolute history line coordinate
    if (_state.screenType != screen_type::Primary)
        return nullopt;
    if (*startLine <= -*historyLineCount())
        return nullopt;

    startLine = min(startLine, boxed_cast<line_offset>(_settings.pageSize.lines - 1));

    for (line_offset i = startLine - 1; i >= -boxed_cast<line_offset>(historyLineCount()); --i)
        if (_grid.lineAt(i).marked())
            return { i };

    return nullopt;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
optional<line_offset> Screen<Cell>::findMarkerDownwards(line_offset startLine) const
{
    if (_state.screenType != screen_type::Primary)
        return nullopt;

    auto const top = std::clamp(startLine,
                                -boxed_cast<line_offset>(historyLineCount()),
                                +boxed_cast<line_offset>(_settings.pageSize.lines) - 1);

    auto const bottom = line_offset(0);

    for (line_offset i = top + 1; i <= bottom; ++i)
        if (_grid.lineAt(i).marked())
            return { i };

    return nullopt;
}

// {{{ tabs related
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::clearAllTabs()
{
    _state.tabs.clear();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::clearTabUnderCursor()
{
    // populate tabs vector in case of default tab width is used (until now).
    if (_state.tabs.empty() && *TabWidth != 0)
        for (auto column = boxed_cast<column_offset>(TabWidth);
             column < boxed_cast<column_offset>(_settings.pageSize.columns);
             column += boxed_cast<column_offset>(TabWidth))
            _state.tabs.emplace_back(column - 1);

    // erase the specific tab underneath
    for (auto i = begin(_state.tabs); i != end(_state.tabs); ++i)
    {
        if (*i == realCursorPosition().column)
        {
            _state.tabs.erase(i);
            break;
        }
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::setTabUnderCursor()
{
    _state.tabs.emplace_back(realCursorPosition().column);
    sort(begin(_state.tabs), end(_state.tabs));
}
// }}}

// {{{ others
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorTo(line_offset line, column_offset column)
{
    auto const [originAppliedLine, originAppliedColumn] = [&]() {
        if (!_cursor.originMode)
            return pair { line, column };
        else
            return pair { line + getMargin().vert.from, column + getMargin().hori.from };
    }();

    _cursor.wrapPending = false;
    _cursor.position = clampToScreen({ originAppliedLine, originAppliedColumn });
    updateCursorIterator();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::linefeed(column_offset newColumn)
{
    _cursor.wrapPending = false;
    _cursor.position.column = newColumn;

    if (*realCursorPosition().line == *getMargin().vert.to)
    {
        // TODO(perf) if we know that we text is following this LF
        // (i.e. parser state will be ground state),
        // then invoke scrollUpUninitialized instead
        // and make sure the subsequent text write will
        // possibly also reset remaining grid cells in that line
        // if the incoming text did not write to the full line
        scrollUp(LineCount(1), _cursor.graphicsRendition, getMargin());
    }
    else
    {
        // using moveCursorTo() would embrace code reusage,
        // but due to the fact that it's fully recalculating iterators,
        // it may be faster to just incrementally update them.
        // moveCursorTo({logicalCursorPosition().line + 1, getMargin().horizontal.from});
        _cursor.position.line++;
        updateCursorIterator();
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::scrollUp(LineCount n, graphics_attributes sgr, margin margin)
{
    auto const scrollCount = _grid.scrollUp(n, sgr, margin);
    updateCursorIterator();
    // TODO only call onBufferScrolled if full page margin
    _terminal.onBufferScrolled(scrollCount);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::scrollDown(LineCount n, margin margin)
{
    _grid.scrollDown(n, cursor().graphicsRendition, margin);
    updateCursorIterator();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::setCurrentColumn(column_offset n)
{
    auto const col = _cursor.originMode ? getMargin().hori.from + n : n;
    auto const clampedCol = min(col, boxed_cast<column_offset>(_settings.pageSize.columns) - 1);
    _cursor.wrapPending = false;
    _cursor.position.column = clampedCol;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
string Screen<Cell>::renderMainPageText() const
{
    return _grid.renderMainPageText();
}
// }}}

// {{{ ops
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::linefeed()
{
    // If coming through stdout-fastpipe, the LF acts like CRLF.
    auto const newColumnOffset =
        _state.usingStdoutFastPipe || _terminal.isModeEnabled(ansi_mode::AutomaticNewLine)
            ? getMargin().hori.from
            : _cursor.position.column;
    linefeed(newColumnOffset);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::backspace()
{
    if (_cursor.position.column.value)
        _cursor.position.column--;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::deviceStatusReport()
{
    _terminal.reply("\033[0n");
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::reportCursorPosition()
{
    _terminal.reply("\033[{};{}R", logicalCursorPosition().line + 1, logicalCursorPosition().column + 1);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::reportExtendedCursorPosition()
{
    auto const pageNum = 1;
    _terminal.reply(
        "\033[{};{};{}R", logicalCursorPosition().line + 1, logicalCursorPosition().column + 1, pageNum);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::selectConformanceLevel(vt_type level)
{
    // Don't enforce the selected conformance level, just remember it.
    _state.terminalId = level;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::sendDeviceAttributes()
{
    // See https://vt100.net/docs/vt510-rm/DA1.html

    auto const id = [&]() -> string_view {
        switch (_state.terminalId)
        {
            case vt_type::VT100: return "1";
            case vt_type::VT220:
            case vt_type::VT240: return "62";
            case vt_type::VT320:
            case vt_type::VT330:
            case vt_type::VT340: return "63";
            case vt_type::VT420: return "64";
            case vt_type::VT510:
            case vt_type::VT520:
            case vt_type::VT525: return "65";
        }
        return "1"; // Should never be reached.
    }();

    auto const attrs = to_params(device_attributes::AnsiColor |
                                 // DeviceAttributes::AnsiTextLocator |
                                 device_attributes::CaptureScreenBuffer | device_attributes::Columns132 |
                                 // TODO: DeviceAttributes::NationalReplacementCharacterSets |
                                 device_attributes::RectangularEditing |
                                 // TODO: DeviceAttributes::SelectiveErase |
                                 device_attributes::SixelGraphics |
                                 // TODO: DeviceAttributes::TechnicalCharacters |
                                 device_attributes::UserDefinedKeys);

    _terminal.reply("\033[?{};{}c", id, attrs);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::sendTerminalId()
{
    // Note, this is "Secondary DA".
    // It requests for the terminalID

    // terminal protocol type
    auto const Pp = static_cast<unsigned>(_state.terminalId);

    // version number
    // TODO: (PACKAGE_VERSION_MAJOR * 100 + PACKAGE_VERSION_MINOR) * 100 + PACKAGE_VERSION_MICRO
    auto constexpr Pv =
        (LIBTERMINAL_VERSION_MAJOR * 100 + LIBTERMINAL_VERSION_MINOR) * 100 + LIBTERMINAL_VERSION_PATCH;

    // ROM cardridge registration number (always 0)
    auto constexpr Pc = 0;

    _terminal.reply("\033[>{};{};{}c", Pp, Pv, Pc);
}

// {{{ ED
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::clearToEndOfScreen()
{
    clearToEndOfLine();

    for (auto const lineOffset:
         ::ranges::views::iota(unbox<int>(_cursor.position.line) + 1, unbox<int>(_settings.pageSize.lines)))
    {
        line<Cell>& line = _grid.lineAt(line_offset::cast_from(lineOffset));
        line.reset(_grid.defaultLineFlags(), _cursor.graphicsRendition);
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::clearToBeginOfScreen()
{
    clearToBeginOfLine();

    for (auto const lineOffset: ::ranges::views::iota(0, *_cursor.position.line))
    {
        line<Cell>& line = _grid.lineAt(line_offset::cast_from(lineOffset));
        line.reset(_grid.defaultLineFlags(), _cursor.graphicsRendition);
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::clearScreen()
{
    // Instead of *just* clearing the screen, and thus, losing potential important content,
    // we scroll up by RowCount number of lines, so move it all into history, so the user can scroll
    // up in case the content is still needed.
    scrollUp(_grid.pageSize().lines);
}
// }}}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::eraseCharacters(ColumnCount n)
{
    // Spec: https://vt100.net/docs/vt510-rm/ECH.html
    // It's not clear from the spec how to perform erase when inside margin and number of chars to be erased
    // would go outside margins.
    // TODO: See what xterm does ;-)

    // erase characters from current colum to the right
    auto const columnsAvailable =
        _settings.pageSize.columns - boxed_cast<ColumnCount>(realCursorPosition().column);
    auto const clampedN = unbox<long>(clamp(n, ColumnCount(1), columnsAvailable));

    auto& line = currentLine();
    for (int i = 0; i < clampedN; ++i)
        line.useCellAt(_cursor.position.column + i).reset(_cursor.graphicsRendition);
}

// {{{ DECSEL
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::selectiveEraseToEndOfLine()
{
    if (isFullHorizontalMargins() && _cursor.position.column.value == 0)
        selectiveEraseLine(_cursor.position.line);
    else
        selectiveErase(_cursor.position.line,
                       _cursor.position.column,
                       column_offset::cast_from(_settings.pageSize.columns));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::selectiveEraseToBeginOfLine()
{
    if (isFullHorizontalMargins() && _cursor.position.column.value == _settings.pageSize.columns.value)
        selectiveEraseLine(_cursor.position.line);
    else
        selectiveErase(_cursor.position.line, column_offset(0), _cursor.position.column + 1);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::selectiveEraseLine(line_offset line)
{
    if (containsProtectedCharacters(
            line, column_offset(0), column_offset::cast_from(_settings.pageSize.columns)))
    {
        selectiveErase(line, column_offset(0), column_offset::cast_from(_settings.pageSize.columns));
        return;
    }

    currentLine().reset(_grid.defaultLineFlags(), _cursor.graphicsRendition);

    auto const leftValue = column_offset(0);
    auto const rightValue = boxed_cast<column_offset>(_settings.pageSize.columns - 1);
    auto const area = rect { top(*line), left(*leftValue), bottom(*line), right(*rightValue) };
    _terminal.markRegionDirty(area);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::selectiveErase(line_offset line, column_offset begin, column_offset end)
{
    Cell* i = &at(line, begin);
    Cell const* e = i + unbox<uintptr_t>(end - begin);
    while (i != e)
    {
        if (i->isFlagEnabled(cell_flags::CharacterProtected))
        {
            ++i;
            continue;
        }
        i->reset(_cursor.graphicsRendition);
        ++i;
    }

    auto const leftValue = begin;
    auto const rightValue = end - 1;
    auto const area = rect { top(*line), left(*leftValue), bottom(*line), right(*rightValue) };
    _terminal.markRegionDirty(area);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
bool Screen<Cell>::containsProtectedCharacters(line_offset line, column_offset begin, column_offset end) const
{
    Cell const* i = &at(line, begin);
    Cell const* e = i + unbox<uintptr_t>(end - begin);
    while (i != e)
    {
        if (i->isFlagEnabled(cell_flags::CharacterProtected))
            return true;
        ++i;
    }
    return false;
}
// }}}
// {{{ DECSED
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::selectiveEraseToEndOfScreen()
{
    selectiveEraseToEndOfLine();

    auto const lineStart = unbox<int>(_cursor.position.line) + 1;
    auto const lineEnd = unbox<int>(_settings.pageSize.lines);

    for (auto const lineOffset: ::ranges::views::iota(lineStart, lineEnd))
        selectiveEraseLine(line_offset::cast_from(lineOffset));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::selectiveEraseToBeginOfScreen()
{
    selectiveEraseToBeginOfLine();

    for (auto const lineOffset: ::ranges::views::iota(0, *_cursor.position.line))
        selectiveEraseLine(line_offset::cast_from(lineOffset));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::selectiveEraseScreen()
{
    for (auto const lineOffset: ::ranges::views::iota(0, *_settings.pageSize.lines))
        selectiveEraseLine(line_offset::cast_from(lineOffset));
}
// }}}
// {{{ DECSERA
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::selectiveEraseArea(rect area)
{
    auto const [top, left, bottom, right] = applyOriginMode(area).clampTo(_settings.pageSize);
    assert(unbox<int>(right) <= unbox<int>(_settings.pageSize.columns));
    assert(unbox<int>(bottom) <= unbox<int>(_settings.pageSize.lines));

    if (top.value > bottom.value || left.value > right.value)
        return;

    for (int y = top.value; y <= bottom.value; ++y)
    {
        for (Cell& cell: grid()
                             .lineAt(line_offset::cast_from(y))
                             .useRange(column_offset::cast_from(left),
                                       ColumnCount::cast_from(right.value - left.value + 1)))
        {
            if (!cell.isFlagEnabled(cell_flags::CharacterProtected))
            {
                cell.writeTextOnly(L' ', 1);
                cell.setHyperlink(hyperlink_id(0));
            }
        }
    }
}
// }}}

// {{{ EL
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::clearToEndOfLine()
{
    if (isFullHorizontalMargins() && _cursor.position.column.value == 0)
    {
        currentLine().reset(currentLine().flags(), _cursor.graphicsRendition);
        return;
    }

    Cell* i = &at(_cursor.position);
    Cell* e = i + unbox<int>(_settings.pageSize.columns) - unbox<int>(_cursor.position.column);
    while (i != e)
    {
        i->reset(_cursor.graphicsRendition);
        ++i;
    }

    auto const line = _cursor.position.line;
    auto const leftValue = _cursor.position.column;
    auto const rightValue = boxed_cast<column_offset>(_settings.pageSize.columns - 1);
    auto const area = rect { top(*line), left(*leftValue), bottom(*line), right(*rightValue) };
    _terminal.markRegionDirty(area);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::clearToBeginOfLine()
{
    Cell* i = &at(_cursor.position.line, column_offset(0));
    Cell* e = i + unbox<int>(_cursor.position.column) + 1;
    while (i != e)
    {
        i->reset(_cursor.graphicsRendition);
        ++i;
    }

    auto const line = _cursor.position.line;
    auto const leftValue = column_offset(0);
    auto const rightValue = _cursor.position.column;
    auto const area = rect { top(*line), left(*leftValue), bottom(*line), right(*rightValue) };
    _terminal.markRegionDirty(area);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::clearLine()
{
    currentLine().reset(_grid.defaultLineFlags(), _cursor.graphicsRendition);

    auto const line = _cursor.position.line;
    auto const leftValue = column_offset(0);
    auto const rightValue = boxed_cast<column_offset>(_settings.pageSize.columns - 1);
    auto const area = rect { top(*line), left(*leftValue), bottom(*line), right(*rightValue) };
    _terminal.markRegionDirty(area);
}
// }}}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorToNextLine(LineCount n)
{
    moveCursorTo(logicalCursorPosition().line + n.as<line_offset>(), column_offset(0));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorToPrevLine(LineCount n)
{
    auto const sanitizedN = min(n.as<line_offset>(), logicalCursorPosition().line);
    moveCursorTo(logicalCursorPosition().line - sanitizedN, column_offset(0));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::insertCharacters(ColumnCount n)
{
    if (isCursorInsideMargins())
        insertChars(realCursorPosition().line, n);
}

/// Inserts @p n characters at given line @p lineNo.
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::insertChars(line_offset lineOffset, ColumnCount columnsToInsert)
{
    auto const sanitizedN = min(*columnsToInsert, *getMargin().hori.to - *logicalCursorPosition().column + 1);

    auto column0 = _grid.lineAt(lineOffset).inflatedBuffer().begin() + *realCursorPosition().column;
    auto column1 = _grid.lineAt(lineOffset).inflatedBuffer().begin() + *getMargin().hori.to - sanitizedN + 1;
    auto column2 = _grid.lineAt(lineOffset).inflatedBuffer().begin() + *getMargin().hori.to + 1;

    rotate(column0, column1, column2);

    for (Cell& cell: _grid.lineAt(lineOffset)
                         .useRange(boxed_cast<column_offset>(_cursor.position.column),
                                   ColumnCount::cast_from(sanitizedN)))
    {
        cell.write(_cursor.graphicsRendition, L' ', 1);
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::insertLines(LineCount n)
{
    if (isCursorInsideMargins())
    {
        scrollDown(n,
                   terminal::margin { margin::vertical { _cursor.position.line, getMargin().vert.to },
                                      getMargin().hori });
        updateCursorIterator();
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::insertColumns(ColumnCount n)
{
    if (isCursorInsideMargins())
        for (auto lineNo = getMargin().vert.from; lineNo <= getMargin().vert.to; ++lineNo)
            insertChars(lineNo, n);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::copyArea(rect sourceArea, int page, cell_location targetTopLeft, int targetPage)
{
    (void) page;
    (void) targetPage;

    // The space at https://vt100.net/docs/vt510-rm/DECCRA.html states:
    // "If Pbs is greater than Pts, // or Pls is greater than Prs, the terminal ignores DECCRA."
    //
    // However, the first part "Pbs is greater than Pts" does not make sense.
    if (*sourceArea.bottom < *sourceArea.top || *sourceArea.right < *sourceArea.left)
        return;

    if (*sourceArea.top == *targetTopLeft.line && *sourceArea.left == *targetTopLeft.column)
        // Copy to its own location => no-op.
        return;

    auto const [x0, xInc, xEnd] = [&]() {
        if (*targetTopLeft.column > *sourceArea.left) // moving right
            return std::tuple { *sourceArea.right - *sourceArea.left, -1, -1 };
        else
            return std::tuple { 0, +1, *sourceArea.right - *sourceArea.left + 1 };
    }();

    auto const [y0, yInc, yEnd] = [&]() {
        if (*targetTopLeft.line > *sourceArea.top) // moving down
            return std::tuple { *sourceArea.bottom - *sourceArea.top, -1, -1 };
        else
            return std::tuple { 0, +1, *sourceArea.bottom - *sourceArea.top + 1 };
    }();

    for (auto y = y0; y != yEnd; y += yInc)
    {
        for (auto x = x0; x != xEnd; x += xInc)
        {
            Cell const& sourceCell = at(line_offset::cast_from(*sourceArea.top + y),
                                        column_offset::cast_from(sourceArea.left + x));
            Cell& targetCell = at(line_offset::cast_from(targetTopLeft.line + y),
                                  column_offset::cast_from(targetTopLeft.column + x));
            targetCell = sourceCell;
        }
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::eraseArea(int top, int left, int bottom, int right)
{
    assert(right <= unbox<int>(_settings.pageSize.columns));
    assert(bottom <= unbox<int>(_settings.pageSize.lines));

    if (top > bottom || left > right)
        return;

    for (int y = top; y <= bottom; ++y)
    {
        for (Cell& cell: grid()
                             .lineAt(line_offset::cast_from(y))
                             .useRange(column_offset(left), ColumnCount(right - left + 1)))
        {
            cell.write(_cursor.graphicsRendition, L' ', 1);
        }
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::fillArea(char32_t ch, int top, int left, int bottom, int right)
{
    // "Pch can be any value from 32 to 126 or from 160 to 255."
    if (!(32 <= ch && ch <= 126) && !(160 <= ch && ch <= 255))
        return;

    auto const w = static_cast<uint8_t>(unicode::width(ch));
    for (int y = top; y <= bottom; ++y)
    {
        for (Cell& cell:
             grid()
                 .lineAt(line_offset::cast_from(y))
                 .useRange(column_offset::cast_from(left), ColumnCount::cast_from(right - left + 1)))
        {
            cell.write(cursor().graphicsRendition, ch, w);
        }
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::deleteLines(LineCount n)
{
    if (isCursorInsideMargins())
    {
        scrollUp(n,
                 terminal::margin { margin::vertical { _cursor.position.line, getMargin().vert.to },
                                    getMargin().hori });
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::deleteCharacters(ColumnCount n)
{
    if (isCursorInsideMargins() && *n != 0)
        deleteChars(realCursorPosition().line, realCursorPosition().column, n);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::deleteChars(line_offset lineOffset, column_offset column, ColumnCount columnsToDelete)
{
    auto& line = _grid.lineAt(lineOffset);
    auto lineBuffer = line.cells();

    Cell* left = const_cast<Cell*>(lineBuffer.data() + column.as<size_t>());
    Cell* right = const_cast<Cell*>(lineBuffer.data() + *getMargin().hori.to + 1);
    long const n = min(columnsToDelete.as<long>(), static_cast<long>(std::distance(left, right)));
    Cell* mid = left + n;

    rotate(left, mid, right);

    for (Cell& cell: gsl::make_span(right - n, right))
    {
        cell.write(_cursor.graphicsRendition, L' ', 1);
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::deleteColumns(ColumnCount n)
{
    if (isCursorInsideMargins())
        for (auto lineNo = getMargin().vert.from; lineNo <= getMargin().vert.to; ++lineNo)
            deleteChars(lineNo, realCursorPosition().column, n);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::horizontalTabClear(HorizontalTabClear which)
{
    switch (which)
    {
        case HorizontalTabClear::AllTabs: clearAllTabs(); break;
        case HorizontalTabClear::UnderCursor: clearTabUnderCursor(); break;
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::horizontalTabSet()
{
    setTabUnderCursor();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::setCurrentWorkingDirectory(string const& url)
{
    _state.currentWorkingDirectory = url;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::hyperlink(string id, string uri)
{
    if (uri.empty())
        _cursor.hyperlink = {};
    else
    {
        if (!id.empty())
        {
            _cursor.hyperlink = _state.hyperlinks.hyperlinkIdByUserId(id);
            if (_cursor.hyperlink != hyperlink_id {})
                return;
        }
        _cursor.hyperlink = _state.hyperlinks.nextHyperlinkId++;
        _state.hyperlinks.cache.emplace(
            _cursor.hyperlink, make_shared<hyperlink_info>(hyperlink_info { std::move(id), std::move(uri) }));
    }
    // TODO:
    // Care about eviction.
    // Move hyperlink store into ScreenBuffer, so it gets reset upon every switch into
    // alternate screen (not for main screen!)
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorUp(LineCount n)
{
    _cursor.wrapPending = false;
    auto const sanitizedN = min(n.as<line_offset>(),
                                logicalCursorPosition().line > getMargin().vert.from
                                    ? logicalCursorPosition().line - getMargin().vert.from
                                    : logicalCursorPosition().line);

    _cursor.position.line -= sanitizedN;
    updateCursorIterator();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorDown(LineCount n)
{
    _cursor.wrapPending = false;
    auto const currentLineNumber = logicalCursorPosition().line;
    auto const sanitizedN =
        min(n.as<line_offset>(),
            currentLineNumber <= getMargin().vert.to
                ? getMargin().vert.to - currentLineNumber
                : (boxed_cast<line_offset>(_settings.pageSize.lines) - 1) - currentLineNumber);

    _cursor.position.line += sanitizedN;
    updateCursorIterator();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorForward(ColumnCount n)
{
    _cursor.wrapPending = false;
    _cursor.position.column = min(_cursor.position.column + n.as<column_offset>(), getMargin().hori.to);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorBackward(ColumnCount n)
{
    // even if you move to 80th of 80 columns, it'll first write a char and THEN flag wrap pending
    _cursor.wrapPending = false;

    // TODO: skip cells that in counting when iterating backwards over a wide cell (such as emoji)
    auto const sanitizedN = min(n.as<column_offset>(), _cursor.position.column);
    setCurrentColumn(_cursor.position.column - sanitizedN);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorToColumn(column_offset column)
{
    setCurrentColumn(column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorToBeginOfLine()
{
    setCurrentColumn(column_offset(0));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorToLine(line_offset n)
{
    moveCursorTo(n, _cursor.position.column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::moveCursorToNextTab()
{
    // TODO: I guess something must remember when a \t was added, for proper move-back?
    // TODO: respect HTS/TBC

    static_assert(TabWidth > ColumnCount(0));
    if (!_state.tabs.empty())
    {
        // advance to the next tab
        size_t i = 0;
        while (i < _state.tabs.size() && realCursorPosition().column >= _state.tabs[i])
            ++i;

        auto const currentCursorColumn = logicalCursorPosition().column;

        if (i < _state.tabs.size())
            moveCursorForward(boxed_cast<ColumnCount>(_state.tabs[i] - currentCursorColumn));
        else if (realCursorPosition().column < getMargin().hori.to)
            moveCursorForward(boxed_cast<ColumnCount>(getMargin().hori.to - currentCursorColumn));
    }
    else
    {
        // default tab settings
        if (realCursorPosition().column < getMargin().hori.to)
        {
            auto const n =
                min((TabWidth - boxed_cast<ColumnCount>(_cursor.position.column) % TabWidth),
                    _settings.pageSize.columns - boxed_cast<ColumnCount>(logicalCursorPosition().column));
            moveCursorForward(n);
        }
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::notify(string const& title, string const& content)
{
    _terminal.notify(title, content);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::captureBuffer(LineCount lineCount, bool logicalLines)
{
    // TODO: Unit test case! (for ensuring line numbering and limits are working as expected)

    auto capturedBuffer = std::string();

    // TODO: when capturing lineCount < screenSize.lines, start at the lowest non-empty line.
    auto const relativeStartLine =
        logicalLines ? _grid.computeLogicalLineNumberFromBottom(LineCount::cast_from(lineCount))
                     : unbox<int>(_settings.pageSize.lines - lineCount);
    auto const startLine = line_offset::cast_from(
        clamp(relativeStartLine, -unbox<int>(historyLineCount()), unbox<int>(_settings.pageSize.lines)));

    VTCaptureBufferLog()("Capture buffer: {} lines {}", lineCount, logicalLines ? "logical" : "actual");

    size_t constexpr MaxChunkSize = 4096;
    size_t currentChunkSize = 0;
    auto const pushContent = [&](auto const data) -> void {
        if (data.empty())
            return;
        if (currentChunkSize == 0) // initiate chunk
            _terminal.reply("\033^{};", CaptureBufferCode);
        else if (currentChunkSize + data.size() >= MaxChunkSize)
        {
            VTCaptureBufferLog()("Transferred chunk of {} bytes.", currentChunkSize);
            _terminal.reply("\033\\"); // ST
            _terminal.reply("\033^{};", CaptureBufferCode);
            currentChunkSize = 0;
        }
        _terminal.reply(data);
        currentChunkSize += data.size();
    };
    line_offset const bottomLine = boxed_cast<line_offset>(_settings.pageSize.lines - 1);
    VTCaptureBufferLog()("Capturing buffer. top: {}, bottom: {}", relativeStartLine, bottomLine);

    for (line_offset line = startLine; line <= bottomLine; ++line)
    {
        if (logicalLines && _grid.lineAt(line).wrapped() && !capturedBuffer.empty())
            capturedBuffer.pop_back();

        auto const& lineBuffer = _grid.lineAt(line);
        auto lineCellsTrimmed = lineBuffer.trim_blank_right();
        if (lineCellsTrimmed.empty())
        {
            VTCaptureBufferLog()("Skipping blank line {}", line);
            continue;
        }
        auto const tl = lineCellsTrimmed.size();
        while (!lineCellsTrimmed.empty())
        {
            auto const available = MaxChunkSize - currentChunkSize;
            auto const n = min(available, lineCellsTrimmed.size());
            for (auto const& cell: lineCellsTrimmed.subspan(0, n))
                pushContent(cell.toUtf8());
            lineCellsTrimmed = lineCellsTrimmed.subspan(n);
        }
        VTCaptureBufferLog()("NL ({} len)", tl);
        pushContent("\n"sv);
    }

    if (currentChunkSize != 0)
        _terminal.reply("\033\\"); // ST

    VTCaptureBufferLog()("Capturing buffer finished.");
    _terminal.reply("\033^{};\033\\", CaptureBufferCode); // mark the end
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::cursorForwardTab(tab_stop_count count)
{
    for (int i = 0; i < unbox<int>(count); ++i)
        moveCursorToNextTab();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::cursorBackwardTab(tab_stop_count count)
{
    if (!count)
        return;

    if (!_state.tabs.empty())
    {
        for (unsigned k = 0; k < unbox<unsigned>(count); ++k)
        {
            auto const i =
                std::find_if(rbegin(_state.tabs), rend(_state.tabs), [&](column_offset tabPos) -> bool {
                    return tabPos < logicalCursorPosition().column;
                });
            if (i != rend(_state.tabs))
            {
                // prev tab found -> move to prev tab
                moveCursorToColumn(*i);
            }
            else
            {
                moveCursorToColumn(getMargin().hori.from);
                break;
            }
        }
    }
    else if (TabWidth.value)
    {
        // default tab settings
        if (*_cursor.position.column < *TabWidth)
            moveCursorToBeginOfLine();
        else
        {
            auto const m = (*_cursor.position.column + 1) % *TabWidth;
            auto const n = m ? (*count - 1) * *TabWidth + m : *count * *TabWidth + m;
            moveCursorBackward(ColumnCount(n - 1));
        }
    }
    else
    {
        // no tab stops configured
        moveCursorToBeginOfLine();
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::index()
{
    if (*realCursorPosition().line == *getMargin().vert.to)
        scrollUp(LineCount(1));
    else
        moveCursorDown(LineCount(1));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::reverseIndex()
{
    if (unbox<int>(realCursorPosition().line) == unbox<int>(getMargin().vert.from))
        scrollDown(LineCount(1));
    else
        moveCursorUp(LineCount(1));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::backIndex()
{
    if (realCursorPosition().column == getMargin().hori.from)
        ; // TODO: scrollRight(1);
    else
        moveCursorForward(ColumnCount(1));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::forwardIndex()
{
    if (*realCursorPosition().column == *getMargin().hori.to)
        _grid.scrollLeft(graphics_attributes {}, getMargin());
    else
        moveCursorForward(ColumnCount(1));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::setForegroundColor(color color)
{
    _cursor.graphicsRendition.foregroundColor = color;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::setBackgroundColor(color color)
{
    _cursor.graphicsRendition.backgroundColor = color;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::setUnderlineColor(color color)
{
    _cursor.graphicsRendition.underlineColor = color;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::setCursorStyle(cursor_display display, cursor_shape shape)
{
    _state.cursorDisplay = display;
    _state.cursorShape = shape;

    _terminal.setCursorStyle(display, shape);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::setGraphicsRendition(graphics_rendition rendition)
{
    _terminal.setGraphicsRendition(rendition);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::setMark()
{
    currentLine().setMarked(true);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::saveModes(std::vector<dec_mode> const& modes)
{
    _state.modes.save(modes);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::restoreModes(std::vector<dec_mode> const& modes)
{
    _state.modes.restore(modes);
}

enum class ModeResponse
{ // TODO: respect response 0, 3, 4.
    NotRecognized = 0,
    Set = 1,
    Reset = 2,
    PermanentlySet = 3,
    PermanentlyReset = 4
};

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::requestAnsiMode(unsigned int mode)
{
    ModeResponse const modeResponse =
        isValidAnsiMode(mode)
            ? _terminal.isModeEnabled(static_cast<ansi_mode>(mode)) ? ModeResponse::Set : ModeResponse::Reset
            : ModeResponse::NotRecognized;

    auto const code = toAnsiModeNum(static_cast<ansi_mode>(mode));

    _terminal.reply("\033[{};{}$y", code, static_cast<unsigned>(modeResponse));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::requestDECMode(unsigned int mode)
{
    ModeResponse const modeResponse =
        isValidDECMode(mode)
            ? _terminal.isModeEnabled(static_cast<dec_mode>(mode)) ? ModeResponse::Set : ModeResponse::Reset
            : ModeResponse::NotRecognized;

    auto const code = toDECModeNum(static_cast<dec_mode>(mode));

    _terminal.reply("\033[?{};{}$y", code, static_cast<unsigned>(modeResponse));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::screenAlignmentPattern()
{
    // sets the margins to the extremes of the page
    getMargin().vert.from = line_offset(0);
    getMargin().vert.to = boxed_cast<line_offset>(_settings.pageSize.lines) - line_offset(1);
    getMargin().hori.from = column_offset(0);
    getMargin().hori.to = boxed_cast<column_offset>(_settings.pageSize.columns) - column_offset(1);

    // and moves the cursor to the home position
    moveCursorTo({}, {});

    // fills the complete screen area with a test pattern
    for (auto& line: _grid.mainPage())
    {
        line.fill(_grid.defaultLineFlags(), graphics_attributes {}, U'E', 1);
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::applicationKeypadMode(bool enable)
{
    _terminal.setApplicationkeypadMode(enable);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::designateCharset(charset_table table, charset_id charset)
{
    // TODO: unit test SCS and see if they also behave well with reset/softreset
    // Also, is the cursor shared between the two buffers?
    _cursor.charsets.select(table, charset);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::singleShiftSelect(charset_table table)
{
    // TODO: unit test SS2, SS3
    _cursor.charsets.singleShift(table);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::sixelImage(image_size pixelSize, image::data&& rgbaData)
{
    auto const columnCount =
        ColumnCount::cast_from(ceilf(float(*pixelSize.width) / float(*_state.cellPixelSize.width)));
    auto const lineCount =
        LineCount::cast_from(ceilf(float(*pixelSize.height) / float(*_state.cellPixelSize.height)));
    auto const extent = grid_size { lineCount, columnCount };
    auto const autoScrollAtBottomMargin = !_terminal.isModeEnabled(dec_mode::NoSixelScrolling);
    auto const topLeft = autoScrollAtBottomMargin ? logicalCursorPosition() : cell_location {};

    auto const alignmentPolicy = image_alignment::TopStart;
    auto const resizePolicy = image_resize::NoResize;

    auto const imageOffset = pixel_coordinate {};
    auto const imageSize = pixelSize;

    shared_ptr<image const> imageRef = uploadImage(image_format::RGBA, pixelSize, std::move(rgbaData));
    renderImage(imageRef,
                topLeft,
                extent,
                imageOffset,
                imageSize,
                alignmentPolicy,
                resizePolicy,
                autoScrollAtBottomMargin);

    if (!_terminal.isModeEnabled(dec_mode::SixelCursorNextToGraphic))
        linefeed(topLeft.column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
shared_ptr<image const> Screen<Cell>::uploadImage(image_format format,
                                                  image_size imageSize,
                                                  image::data&& pixmap)
{
    return _state.imagePool.create(format, imageSize, std::move(pixmap));
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::renderImage(shared_ptr<image const> image,
                               cell_location topLeft,
                               grid_size gridSize,
                               pixel_coordinate imageOffset,
                               image_size imageSize,
                               image_alignment alignmentPolicy,
                               image_resize resizePolicy,
                               bool autoScroll)
{
    // TODO: make use of imageOffset
    (void) imageOffset;

    auto const linesAvailable =
        _settings.pageSize.lines - topLeft.line.as<LineCount>() - _terminal.statusLineHeight();
    auto const linesToBeRendered = min(gridSize.lines, linesAvailable);
    auto const columnsAvailable = *_settings.pageSize.columns - *topLeft.column;
    auto const columnsToBeRendered = ColumnCount(min(columnsAvailable, *gridSize.columns));
    auto const gapColor = rgba_color {}; // TODO: _cursor.graphicsRendition.backgroundColor;

    // TODO: make use of imageOffset and imageSize
    auto const rasterizedImage = _state.imagePool.rasterize(
        std::move(image), alignmentPolicy, resizePolicy, gapColor, gridSize, _state.cellPixelSize);
    const auto lastSixelBand = imageSize.height.value % 6;
    const line_offset offset = [&]() {
        auto offset =
            line_offset::cast_from(std::ceil(static_cast<float>(imageSize.height.value - lastSixelBand)
                                             / float(*_state.cellPixelSize.height)))
            - 1 * (lastSixelBand == 0);
        auto const h = imageSize.height.value - 1;
        // VT340 has this behavior where for some heights it text cursor is placed not
        // at the final sixel line but a line above it.
        // See
        // https://github.com/hackerb9/vt340test/blob/main/glitches.md#text-cursor-is-left-one-row-too-high-for-certain-sixel-heights
        if (h % 6 > h % _state.cellPixelSize.height.value)
            return offset - 1;
        return offset;
    }();
    if (*linesToBeRendered)
    {
        for (grid_size::offset const offset: grid_size { linesToBeRendered, columnsToBeRendered })
        {
            Cell& cell = at(topLeft + offset);
            cell.setImageFragment(rasterizedImage, cell_location { offset.line, offset.column });
            cell.setHyperlink(_cursor.hyperlink);
        };
        moveCursorTo(topLeft.line + offset, topLeft.column);
    }

    // If there're lines to be rendered missing (because it didn't fit onto the screen just yet)
    // AND iff sixel !sixelScrolling  is enabled, then scroll as much as needed to render the remaining
    // lines.
    if (linesToBeRendered != gridSize.lines && autoScroll)
    {
        auto const remainingLineCount = gridSize.lines - linesToBeRendered;
        for (auto const lineOffset: crispy::times(*remainingLineCount))
        {
            linefeed(topLeft.column);
            for (auto const columnOffset: crispy::views::iota_as<column_offset>(*columnsToBeRendered))
            {
                auto const offset =
                    cell_location { boxed_cast<line_offset>(linesToBeRendered) + lineOffset, columnOffset };
                Cell& cell =
                    at(boxed_cast<line_offset>(_settings.pageSize.lines - _terminal.statusLineHeight()) - 1,
                       topLeft.column + columnOffset);
                cell.setImageFragment(rasterizedImage, offset);
                cell.setHyperlink(_cursor.hyperlink);
            };
        }
    }
    // move ansi text cursor to position of the sixel cursor
    moveCursorToColumn(topLeft.column);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::requestDynamicColor(dynamic_color_name name)
{
    auto const color = [&]() -> optional<rgb_color> {
        switch (name)
        {
            case dynamic_color_name::DefaultForegroundColor: return _state.colorPalette.defaultForeground;
            case dynamic_color_name::DefaultBackgroundColor: return _state.colorPalette.defaultBackground;
            case dynamic_color_name::TextCursorColor:
                if (holds_alternative<cell_foreground_color>(_state.colorPalette.cursor.color))
                    return _state.colorPalette.defaultForeground;
                else if (holds_alternative<cell_background_color>(_state.colorPalette.cursor.color))
                    return _state.colorPalette.defaultBackground;
                else
                    return get<rgb_color>(_state.colorPalette.cursor.color);
            case dynamic_color_name::MouseForegroundColor: return _state.colorPalette.mouseForeground;
            case dynamic_color_name::MouseBackgroundColor: return _state.colorPalette.mouseBackground;
            case dynamic_color_name::HighlightForegroundColor:
                if (holds_alternative<rgb_color>(_state.colorPalette.selection.foreground))
                    return get<rgb_color>(_state.colorPalette.selection.foreground);
                else
                    return nullopt;
            case dynamic_color_name::HighlightBackgroundColor:
                if (holds_alternative<rgb_color>(_state.colorPalette.selection.background))
                    return get<rgb_color>(_state.colorPalette.selection.background);
                else
                    return nullopt;
        }
        return nullopt; // should never happen
    }();

    if (color.has_value())
    {
        _terminal.reply(
            "\033]{};{}\033\\", setDynamicColorCommand(name), setDynamicColorValue(color.value()));
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::requestPixelSize(RequestPixelSize area)
{
    switch (area)
    {
        case RequestPixelSize::WindowArea: [[fallthrough]]; // TODO
        case RequestPixelSize::TextArea: {
            // Result is CSI  4 ;  height ;  width t
            _terminal.reply("\033[4;{};{}t", pixelSize().height, pixelSize().width);
            break;
        }
        case RequestPixelSize::CellArea:
            // Result is CSI  6 ;  height ;  width t
            _terminal.reply("\033[6;{};{}t", _state.cellPixelSize.height, _state.cellPixelSize.width);
            break;
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::requestCharacterSize(RequestPixelSize area)
{
    switch (area)
    {
        case RequestPixelSize::TextArea:
            _terminal.reply("\033[8;{};{}t", _settings.pageSize.lines, _settings.pageSize.columns);
            break;
        case RequestPixelSize::WindowArea:
            _terminal.reply("\033[9;{};{}t", _settings.pageSize.lines, _settings.pageSize.columns);
            break;
        case RequestPixelSize::CellArea:
            Guarantee(false
                      && "Screen.requestCharacterSize: Doesn't make sense, and cannot be called, therefore, "
                         "fortytwo.");
            break;
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::requestStatusString(RequestStatusString value)
{
    // xterm responds with DCS 1 $ r Pt ST for valid requests
    // or DCS 0 $ r Pt ST for invalid requests.
    auto const response = [&](RequestStatusString value) -> optional<string> {
        switch (value)
        {
            case RequestStatusString::DECSCL: {
                auto level = 61;
                switch (_state.terminalId)
                {
                    case vt_type::VT525:
                    case vt_type::VT520:
                    case vt_type::VT510: level = 65; break;
                    case vt_type::VT420: level = 64; break;
                    case vt_type::VT340:
                    case vt_type::VT330:
                    case vt_type::VT320: level = 63; break;
                    case vt_type::VT240:
                    case vt_type::VT220: level = 62; break;
                    case vt_type::VT100: level = 61; break;
                }

                auto const c1TransmittionMode = control_transmission_mode::S7C1T;
                auto const c1t = c1TransmittionMode == control_transmission_mode::S7C1T ? 1 : 0;

                return fmt::format("{};{}\"p", level, c1t);
            }
            case RequestStatusString::DECSCUSR: // Set cursor style (DECSCUSR), VT520
            {
                int const blinkingOrSteady = _state.cursorDisplay == cursor_display::Steady ? 1 : 0;
                int const shape = [&]() {
                    switch (_state.cursorShape)
                    {
                        case cursor_shape::Block: return 1;
                        case cursor_shape::Underscore: return 3;
                        case cursor_shape::Bar: return 5;
                        case cursor_shape::Rectangle: return 7;
                    }
                    return 1;
                }();
                return fmt::format("{} q", shape + blinkingOrSteady);
            }
            case RequestStatusString::DECSLPP:
                // Ps >= 2 4  -> Resize to Ps lines (DECSLPP), VT340 and VT420.
                // xterm adapts this by resizing its window.
                if (*_settings.pageSize.lines >= 24)
                    return fmt::format("{}t", _settings.pageSize.lines);
                errorlog()("Requesting device status for {} not with line count < 24 is undefined.");
                return nullopt;
            case RequestStatusString::DECSTBM:
                return fmt::format("{};{}r", 1 + *getMargin().vert.from, *getMargin().vert.to);
            case RequestStatusString::DECSLRM:
                return fmt::format("{};{}s", 1 + *getMargin().hori.from, *getMargin().hori.to);
            case RequestStatusString::DECSCPP:
                // EXTENSION: Usually DECSCPP only knows about 80 and 132, but we take any.
                return fmt::format("{}|$", _settings.pageSize.columns);
            case RequestStatusString::DECSNLS: return fmt::format("{}*|", _settings.pageSize.lines);
            case RequestStatusString::SGR:
                return fmt::format("0;{}m", vtSequenceParameterString(_cursor.graphicsRendition));
            case RequestStatusString::DECSCA: {
                auto const isProtected = _cursor.graphicsRendition.flags & cell_flags::CharacterProtected;
                return fmt::format("{}\"q", isProtected ? 1 : 2);
            }
            case RequestStatusString::DECSASD:
                switch (_state.activeStatusDisplay)
                {
                    case active_status_display::Main: return "0$}";
                    case active_status_display::StatusLine: return "1$}";
                    case active_status_display::IndicatorStatusLine: return "2$}"; // XXX This is not standard
                }
                break;
            case RequestStatusString::DECSSDT:
                switch (_state.statusDisplayType)
                {
                    case status_display_type::None: return "0$~";
                    case status_display_type::Indicator: return "1$~";
                    case status_display_type::HostWritable: return "2$~";
                }
                break;
        }
        return nullopt;
    }(value);

    _terminal.reply("\033P{}$r{}\033\\", response.has_value() ? 1 : 0, response.value_or(""), "\"p");
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::requestTabStops()
{
    // Response: `DCS 2 $ u Pt ST`
    ostringstream dcs;
    dcs << "\033P2$u"sv; // DCS
    if (!_state.tabs.empty())
    {
        for (size_t const i: times(_state.tabs.size()))
        {
            if (i)
                dcs << '/';
            dcs << *_state.tabs[i] + 1;
        }
    }
    else if (*TabWidth != 0)
    {
        dcs << 1;
        for (auto column = *TabWidth + 1; column <= *_settings.pageSize.columns; column += *TabWidth)
            dcs << '/' << column;
    }
    dcs << "\033\\"sv; // ST

    _terminal.reply(dcs.str());
}

namespace
{
    std::string asHex(std::string_view value)
    {
        std::string output;
        for (char const ch: value)
            output += fmt::format("{:02X}", unsigned(ch));
        return output;
    }
} // namespace

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::requestCapability(std::string_view name)
{
    if (booleanCapability(name))
        _terminal.reply("\033P1+r{}\033\\", toHexString(name));
    else if (auto const value = numericCapability(name); value != database::npos)
    {
        auto hexValue = fmt::format("{:X}", value);
        if (hexValue.size() % 2)
            hexValue.insert(hexValue.begin(), '0');
        _terminal.reply("\033P1+r{}={}\033\\", toHexString(name), hexValue);
    }
    else if (auto const value = stringCapability(name); !value.empty())
        _terminal.reply("\033P1+r{}={}\033\\", toHexString(name), asHex(value));
    else
        _terminal.reply("\033P0+r\033\\");
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::requestCapability(capabilities::code c)
{
    if (booleanCapability(c))
        _terminal.reply("\033P1+r{}\033\\", c.hex());
    else if (auto const value = numericCapability(c); value >= 0)
    {
        auto hexValue = fmt::format("{:X}", value);
        if (hexValue.size() % 2)
            hexValue.insert(hexValue.begin(), '0');
        _terminal.reply("\033P1+r{}={}\033\\", c.hex(), hexValue);
    }
    else if (auto const value = stringCapability(c); !value.empty())
        _terminal.reply("\033P1+r{}={}\033\\", c.hex(), asHex(value));
    else
        _terminal.reply("\033P0+r\033\\");
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::resetDynamicColor(dynamic_color_name name)
{
    switch (name)
    {
        case dynamic_color_name::DefaultForegroundColor:
            _state.colorPalette.defaultForeground = _state.defaultColorPalette.defaultForeground;
            break;
        case dynamic_color_name::DefaultBackgroundColor:
            _state.colorPalette.defaultBackground = _state.defaultColorPalette.defaultBackground;
            break;
        case dynamic_color_name::TextCursorColor:
            _state.colorPalette.cursor = _state.defaultColorPalette.cursor;
            break;
        case dynamic_color_name::MouseForegroundColor:
            _state.colorPalette.mouseForeground = _state.defaultColorPalette.mouseForeground;
            break;
        case dynamic_color_name::MouseBackgroundColor:
            _state.colorPalette.mouseBackground = _state.defaultColorPalette.mouseBackground;
            break;
        case dynamic_color_name::HighlightForegroundColor:
            _state.colorPalette.selection.foreground = _state.defaultColorPalette.selection.foreground;
            break;
        case dynamic_color_name::HighlightBackgroundColor:
            _state.colorPalette.selection.background = _state.defaultColorPalette.selection.background;
            break;
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::setDynamicColor(dynamic_color_name name, rgb_color color)
{
    switch (name)
    {
        case dynamic_color_name::DefaultForegroundColor: _state.colorPalette.defaultForeground = color; break;
        case dynamic_color_name::DefaultBackgroundColor: _state.colorPalette.defaultBackground = color; break;
        case dynamic_color_name::TextCursorColor: _state.colorPalette.cursor.color = color; break;
        case dynamic_color_name::MouseForegroundColor: _state.colorPalette.mouseForeground = color; break;
        case dynamic_color_name::MouseBackgroundColor: _state.colorPalette.mouseBackground = color; break;
        case dynamic_color_name::HighlightForegroundColor:
            _state.colorPalette.selection.foreground = color;
            break;
        case dynamic_color_name::HighlightBackgroundColor:
            _state.colorPalette.selection.background = color;
            break;
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::inspect()
{
    _terminal.inspect();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::inspect(std::string const& message, std::ostream& os) const
{
    auto const hline = [&]() {
        for_each(crispy::times(*_settings.pageSize.columns), [&](auto) { os << '='; });
        os << endl;
    };

    auto const gridInfoLine = [&](Grid<Cell> const& grid) {
        return fmt::format("main page lines: scrollback cur {} max {}, main page lines {}, used lines "
                           "{}, zero index {}\n",
                           grid.historyLineCount(),
                           grid.maxHistoryLineCount(),
                           grid.pageSize().lines,
                           grid.linesUsed(),
                           grid.zero_index());
    };

    if (!message.empty())
    {
        hline();
        os << "\033[1;37;41m" << message << "\033[m" << endl;
        hline();
    }

    os << fmt::format("Rendered screen at the time of failure\n");
    os << fmt::format("main page size       : {}\n", _settings.pageSize);
    os << fmt::format("history line count   : {} (max {})\n",
                      _terminal.primaryScreen().historyLineCount(),
                      _terminal.maxHistoryLineCount());
    os << fmt::format("cursor position      : {}\n", _cursor);
    os << fmt::format("vertical margins     : {}\n", getMargin().vert);
    os << fmt::format("horizontal margins   : {}\n", getMargin().hori);
    os << gridInfoLine(grid());

    hline();
    os << screenshot([this](line_offset lineNo) -> string {
        // auto const absoluteLine = _grid.toAbsoluteLine(lineNo);
        return fmt::format("{} {:>4}: {}",
                           _grid.lineAt(lineNo).isTrivialBuffer() ? "|" : ":",
                           lineNo.value,
                           _grid.lineAt(lineNo).flags());
    });
    hline();
    _state.imagePool.inspect(os);
    hline();

    // TODO: print more useful debug information
    // - screen size
    // - left/right margin
    // - top/down margin
    // - cursor position
    // - autoWrap
    // - ... other output related modes
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::smGraphics(XtSmGraphics::Item item, XtSmGraphics::Action action, XtSmGraphics::Value value)
{
    using Item = XtSmGraphics::Item;
    using Action = XtSmGraphics::Action;

    constexpr auto NumberOfColorRegistersItem = 1;
    constexpr auto SixelItem = 2;

    constexpr auto Success = 0;
    constexpr auto Failure = 3;

    switch (item)
    {
        case Item::NumberOfColorRegisters:
            switch (action)
            {
                case Action::Read: {
                    auto const value = _state.imageColorPalette->size();
                    _terminal.reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::ReadLimit: {
                    auto const value = _state.imageColorPalette->maxSize();
                    _terminal.reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::ResetToDefault: {
                    auto const value = _state.maxImageColorRegisters;
                    _state.imageColorPalette->setSize(value);
                    _terminal.reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Success, value);
                    break;
                }
                case Action::SetToValue:
                    visit(overloaded {
                              [&](int number) {
                                  _state.imageColorPalette->setSize(static_cast<unsigned>(number));
                                  _terminal.reply(
                                      "\033[?{};{};{}S", NumberOfColorRegistersItem, Success, number);
                              },
                              [&](image_size) {
                                  _terminal.reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Failure, 0);
                              },
                              [&](monostate) {
                                  _terminal.reply("\033[?{};{};{}S", NumberOfColorRegistersItem, Failure, 0);
                              },
                          },
                          value);
                    break;
            }
            break;

        case Item::SixelGraphicsGeometry:
            switch (action)
            {
                case Action::Read: {
                    auto const viewportSize = pixelSize();
                    _terminal.reply("\033[?{};{};{};{}S",
                                    SixelItem,
                                    Success,
                                    min(viewportSize.width, _state.effectiveImageCanvasSize.width),
                                    min(viewportSize.height, _state.effectiveImageCanvasSize.height));
                }
                break;
                case Action::ReadLimit:
                    _terminal.reply("\033[?{};{};{};{}S",
                                    SixelItem,
                                    Success,
                                    _settings.maxImageSize.width,
                                    _settings.maxImageSize.height);
                    break;
                case Action::ResetToDefault:
                    // The limit is the default at the same time.
                    _state.effectiveImageCanvasSize = _settings.maxImageSize;
                    break;
                case Action::SetToValue:
                    if (holds_alternative<image_size>(value))
                    {
                        auto size = get<image_size>(value);
                        size.width = min(size.width, _settings.maxImageSize.width);
                        size.height = min(size.height, _settings.maxImageSize.height);
                        _state.effectiveImageCanvasSize = size;
                        _terminal.reply("\033[?{};{};{};{}S", SixelItem, Success, size.width, size.height);
                    }
                    else
                        _terminal.reply("\033[?{};{};{}S", SixelItem, Failure, 0);
                    break;
            }
            break;

        case Item::ReGISGraphicsGeometry: // Surely, we don't do ReGIS just yet. :-)
            break;
    }
}
// }}}

// {{{ impl namespace (some command generator helpers)
namespace impl
{
    namespace
    {
        ApplyResult setAnsiMode(sequence const& seq, size_t modeIndex, bool enable, Terminal& term)
        {
            switch (seq.param(modeIndex))
            {
                case 2: // (AM) Keyboard Action Mode
                    return ApplyResult::Unsupported;
                case 4: // (IRM) Insert Mode
                    term.setMode(ansi_mode::Insert, enable);
                    return ApplyResult::Ok;
                case 12: // (SRM) Send/Receive Mode
                case 20: // (LNM) Automatic Newline
                default: return ApplyResult::Unsupported;
            }
        }

        optional<dec_mode> toDECMode(unsigned value)
        {
            switch (value)
            {
                case 1: return dec_mode::UseApplicationCursorKeys;
                case 2: return dec_mode::DesignateCharsetUSASCII;
                case 3: return dec_mode::Columns132;
                case 4: return dec_mode::SmoothScroll;
                case 5: return dec_mode::ReverseVideo;
                case 6: return dec_mode::Origin;
                case 7: return dec_mode::AutoWrap;
                // TODO: Ps = 8  -> Auto-repeat Keys (DECARM), VT100.
                case 9: return dec_mode::MouseProtocolX10;
                case 10: return dec_mode::ShowToolbar;
                case 12: return dec_mode::BlinkingCursor;
                case 19: return dec_mode::PrinterExtend;
                case 25: return dec_mode::VisibleCursor;
                case 30: return dec_mode::ShowScrollbar;
                // TODO: Ps = 3 5  -> Enable font-shifting functions (rxvt).
                // IGNORE? Ps = 3 8  -> Enter Tektronix Mode (DECTEK), VT240, xterm.
                // TODO: Ps = 4 0  -> Allow 80 -> 132 Mode, xterm.
                case 40: return dec_mode::AllowColumns80to132;
                // IGNORE: Ps = 4 1  -> more(1) fix (see curses resource).
                // TODO: Ps = 4 2  -> Enable National Replacement Character sets (DECNRCM), VT220.
                // TODO: Ps = 4 4  -> Turn On Margin Bell, xterm.
                // TODO: Ps = 4 5  -> Reverse-wraparound Mode, xterm.
                case 46: return dec_mode::DebugLogging;
                case 47: return dec_mode::UseAlternateScreen;
                // TODO: Ps = 6 6  -> Application keypad (DECNKM), VT320.
                // TODO: Ps = 6 7  -> Backarrow key sends backspace (DECBKM), VT340, VT420.  This sets the
                // backarrowKey resource to "true".
                case 69: return dec_mode::LeftRightMargin;
                case 80: return dec_mode::NoSixelScrolling;
                case 1000: return dec_mode::MouseProtocolNormalTracking;
                case 1001: return dec_mode::MouseProtocolHighlightTracking;
                case 1002: return dec_mode::MouseProtocolButtonTracking;
                case 1003: return dec_mode::MouseProtocolAnyEventTracking;
                case 1004: return dec_mode::FocusTracking;
                case 1005: return dec_mode::MouseExtended;
                case 1006: return dec_mode::MouseSGR;
                case 1007: return dec_mode::MouseAlternateScroll;
                case 1015: return dec_mode::MouseURXVT;
                case 1016: return dec_mode::MouseSGRPixels;
                case 1047: return dec_mode::UseAlternateScreen;
                case 1048: return dec_mode::SaveCursor;
                case 1049: return dec_mode::ExtendedAltScreen;
                case 2004: return dec_mode::BracketedPaste;
                case 2026: return dec_mode::BatchedRendering;
                case 2027: return dec_mode::Unicode;
                case 2028: return dec_mode::TextReflow;
                case 2029: return dec_mode::MousePassiveTracking;
                case 2030: return dec_mode::ReportGridCellSelection;
                case 8452: return dec_mode::SixelCursorNextToGraphic;
            }
            return nullopt;
        }

        ApplyResult setModeDEC(sequence const& seq, size_t modeIndex, bool enable, Terminal& term)
        {
            if (auto const modeOpt = toDECMode(seq.param(modeIndex)); modeOpt.has_value())
            {
                term.setMode(modeOpt.value(), enable);
                return ApplyResult::Ok;
            }
            return ApplyResult::Invalid;
        }

        optional<rgb_color> parseColor(string_view const& value)
        {
            try
            {
                // "rgb:RR/GG/BB"
                //  0123456789a
                if (value.size() == 12 && value.substr(0, 4) == "rgb:" && value[6] == '/' && value[9] == '/')
                {
                    auto const r = crispy::to_integer<16, uint8_t>(value.substr(4, 2));
                    auto const g = crispy::to_integer<16, uint8_t>(value.substr(7, 2));
                    auto const b = crispy::to_integer<16, uint8_t>(value.substr(10, 2));
                    return rgb_color { r.value(), g.value(), b.value() };
                }

                // "#RRGGBB"
                if (value.size() == 7 && value[0] == '#')
                {
                    auto const r = crispy::to_integer<16, uint8_t>(value.substr(1, 2));
                    auto const g = crispy::to_integer<16, uint8_t>(value.substr(3, 2));
                    auto const b = crispy::to_integer<16, uint8_t>(value.substr(5, 2));
                    return rgb_color { r.value(), g.value(), b.value() };
                }

                // "#RGB"
                if (value.size() == 4 && value[0] == '#')
                {
                    auto const r = crispy::to_integer<16, uint8_t>(value.substr(1, 1));
                    auto const g = crispy::to_integer<16, uint8_t>(value.substr(2, 1));
                    auto const b = crispy::to_integer<16, uint8_t>(value.substr(3, 1));
                    auto const rr = static_cast<uint8_t>(r.value() << 4);
                    auto const gg = static_cast<uint8_t>(g.value() << 4);
                    auto const bb = static_cast<uint8_t>(b.value() << 4);
                    return rgb_color { rr, gg, bb };
                }

                return std::nullopt;
            }
            catch (...)
            {
                // that will be a formatting error in stoul() then.
                return std::nullopt;
            }
        }

        color parseColor(sequence const& seq, size_t* pi)
        {
            // We are at parameter index `i`.
            //
            // It may now follow:
            // - ":2::r:g:b"        RGB color
            // - ":3:F:C:M:Y"       CMY color  (F is scaling factor, what is max? 100 or 255?)
            // - ":4:F:C:M:Y:K"     CMYK color (F is scaling factor, what is max? 100 or 255?)
            // - ":5:P"
            // Sub-parameters can also be delimited with ';' and thus are no sub-parameters per-se.
            size_t i = *pi;
            auto const len = seq.subParameterCount(i);
            if (seq.subParameterCount(i) >= 1)
            {
                switch (seq.param(i + 1))
                {
                    case 2: // ":2::R:G:B" and ":2:R:G:B"
                    {
                        if (len == 4 || len == 5)
                        {
                            // NB: subparam(i, 1) may be ignored
                            auto const r = seq.subparam(i, len - 2);
                            auto const g = seq.subparam(i, len - 1);
                            auto const b = seq.subparam(i, len - 0);
                            if (r <= 255 && g <= 255 && b <= 255)
                            {
                                *pi += len;
                                return color { rgb_color { static_cast<uint8_t>(r),
                                                           static_cast<uint8_t>(g),
                                                           static_cast<uint8_t>(b) } };
                            }
                        }
                        break;
                    }
                    case 3: // ":3:F:C:M:Y" (TODO)
                    case 4: // ":4:F:C:M:Y:K" (TODO)
                        *pi += len;
                        break;
                    case 5: // ":5:P"
                        if (auto const P = seq.subparam(i, 2); P <= 255)
                        {
                            *pi += len;
                            return static_cast<indexed_color>(P);
                        }
                        break;
                    default:
                        // XXX invalid sub parameter
                        break;
                }
            }

            // Compatibility mode, colors using ';' instead of ':'.
            if (i + 1 < seq.parameterCount())
            {
                ++i;
                auto const mode = seq.param(i);
                if (mode == 5)
                {
                    if (i + 1 < seq.parameterCount())
                    {
                        ++i;
                        auto const value = seq.param(i);
                        if (i <= 255)
                        {
                            *pi = i;
                            return static_cast<indexed_color>(value);
                        }
                        else
                        {
                        } // TODO: seq.logInvalidCSI("Invalid color indexing.");
                    }
                    else
                    {
                    } // TODO: seq.logInvalidCSI("Missing color index.");
                }
                else if (mode == 2)
                {
                    if (i + 3 < seq.parameterCount())
                    {
                        auto const r = seq.param(i + 1);
                        auto const g = seq.param(i + 2);
                        auto const b = seq.param(i + 3);
                        i += 3;
                        if (r <= 255 && g <= 255 && b <= 255)
                        {
                            *pi = i;
                            return rgb_color { static_cast<uint8_t>(r),
                                               static_cast<uint8_t>(g),
                                               static_cast<uint8_t>(b) };
                        }
                        else
                        {
                        } // TODO: seq.logInvalidCSI("RGB color out of range.");
                    }
                    else
                    {
                    } // TODO: seq.logInvalidCSI("Invalid color mode.");
                }
                else
                {
                } // TODO: seq.logInvalidCSI("Invalid color mode.");
            }
            else
            {
            } // TODO: seq.logInvalidCSI("Invalid color indexing.");

            // failure case, skip this argument
            *pi = i + 1;
            return color {};
        }

        template <typename Target>
        CRISPY_REQUIRES((CellConcept<Target> || std::is_same_v<Target, Terminal>) )
        ApplyResult applySGR(Target& target, sequence const& seq, size_t parameterStart, size_t parameterEnd)
        {
            if (parameterStart == parameterEnd)
            {
                target.setGraphicsRendition(graphics_rendition::Reset);
                return ApplyResult::Ok;
            }

            for (size_t i = parameterStart; i < parameterEnd; ++i)
            {
                switch (seq.param(i))
                {
                    case 0: target.setGraphicsRendition(graphics_rendition::Reset); break;
                    case 1: target.setGraphicsRendition(graphics_rendition::Bold); break;
                    case 2: target.setGraphicsRendition(graphics_rendition::Faint); break;
                    case 3: target.setGraphicsRendition(graphics_rendition::Italic); break;
                    case 4:
                        if (seq.subParameterCount(i) == 1)
                        {
                            switch (seq.subparam(i, 1))
                            {
                                case 0: target.setGraphicsRendition(graphics_rendition::NoUnderline); break;
                                case 1: target.setGraphicsRendition(graphics_rendition::Underline); break;
                                case 2:
                                    target.setGraphicsRendition(graphics_rendition::DoublyUnderlined);
                                    break;
                                case 3:
                                    target.setGraphicsRendition(graphics_rendition::CurlyUnderlined);
                                    break;
                                case 4:
                                    target.setGraphicsRendition(graphics_rendition::DottedUnderline);
                                    break;
                                case 5:
                                    target.setGraphicsRendition(graphics_rendition::DashedUnderline);
                                    break;
                                default: target.setGraphicsRendition(graphics_rendition::Underline); break;
                            }
                            ++i;
                        }
                        else
                            target.setGraphicsRendition(graphics_rendition::Underline);
                        break;
                    case 5: target.setGraphicsRendition(graphics_rendition::Blinking); break;
                    case 6: target.setGraphicsRendition(graphics_rendition::RapidBlinking); break;
                    case 7: target.setGraphicsRendition(graphics_rendition::Inverse); break;
                    case 8: target.setGraphicsRendition(graphics_rendition::Hidden); break;
                    case 9: target.setGraphicsRendition(graphics_rendition::CrossedOut); break;
                    case 21: target.setGraphicsRendition(graphics_rendition::DoublyUnderlined); break;
                    case 22: target.setGraphicsRendition(graphics_rendition::Normal); break;
                    case 23: target.setGraphicsRendition(graphics_rendition::NoItalic); break;
                    case 24: target.setGraphicsRendition(graphics_rendition::NoUnderline); break;
                    case 25: target.setGraphicsRendition(graphics_rendition::NoBlinking); break;
                    case 27: target.setGraphicsRendition(graphics_rendition::NoInverse); break;
                    case 28: target.setGraphicsRendition(graphics_rendition::NoHidden); break;
                    case 29: target.setGraphicsRendition(graphics_rendition::NoCrossedOut); break;
                    case 30: target.setForegroundColor(indexed_color::Black); break;
                    case 31: target.setForegroundColor(indexed_color::Red); break;
                    case 32: target.setForegroundColor(indexed_color::Green); break;
                    case 33: target.setForegroundColor(indexed_color::Yellow); break;
                    case 34: target.setForegroundColor(indexed_color::Blue); break;
                    case 35: target.setForegroundColor(indexed_color::Magenta); break;
                    case 36: target.setForegroundColor(indexed_color::Cyan); break;
                    case 37: target.setForegroundColor(indexed_color::White); break;
                    case 38: target.setForegroundColor(parseColor(seq, &i)); break;
                    case 39: target.setForegroundColor(DefaultColor()); break;
                    case 40: target.setBackgroundColor(indexed_color::Black); break;
                    case 41: target.setBackgroundColor(indexed_color::Red); break;
                    case 42: target.setBackgroundColor(indexed_color::Green); break;
                    case 43: target.setBackgroundColor(indexed_color::Yellow); break;
                    case 44: target.setBackgroundColor(indexed_color::Blue); break;
                    case 45: target.setBackgroundColor(indexed_color::Magenta); break;
                    case 46: target.setBackgroundColor(indexed_color::Cyan); break;
                    case 47: target.setBackgroundColor(indexed_color::White); break;
                    case 48: target.setBackgroundColor(parseColor(seq, &i)); break;
                    case 49: target.setBackgroundColor(DefaultColor()); break;
                    case 51: target.setGraphicsRendition(graphics_rendition::Framed); break;
                    case 53: target.setGraphicsRendition(graphics_rendition::Overline); break;
                    case 54: target.setGraphicsRendition(graphics_rendition::NoFramed); break;
                    case 55: target.setGraphicsRendition(graphics_rendition::NoOverline); break;
                    // 58 is reserved, but used for setting underline/decoration colors by some other VTEs
                    // (such as mintty, kitty, libvte)
                    case 58: target.setUnderlineColor(parseColor(seq, &i)); break;
                    case 90: target.setForegroundColor(bright_color::Black); break;
                    case 91: target.setForegroundColor(bright_color::Red); break;
                    case 92: target.setForegroundColor(bright_color::Green); break;
                    case 93: target.setForegroundColor(bright_color::Yellow); break;
                    case 94: target.setForegroundColor(bright_color::Blue); break;
                    case 95: target.setForegroundColor(bright_color::Magenta); break;
                    case 96: target.setForegroundColor(bright_color::Cyan); break;
                    case 97: target.setForegroundColor(bright_color::White); break;
                    case 100: target.setBackgroundColor(bright_color::Black); break;
                    case 101: target.setBackgroundColor(bright_color::Red); break;
                    case 102: target.setBackgroundColor(bright_color::Green); break;
                    case 103: target.setBackgroundColor(bright_color::Yellow); break;
                    case 104: target.setBackgroundColor(bright_color::Blue); break;
                    case 105: target.setBackgroundColor(bright_color::Magenta); break;
                    case 106: target.setBackgroundColor(bright_color::Cyan); break;
                    case 107: target.setBackgroundColor(bright_color::White); break;
                    default: break; // TODO: logInvalidCSI("Invalid SGR number: {}", seq.param(i));
                }
            }
            return ApplyResult::Ok;
        }

        template <typename Cell>
        ApplyResult CPR(sequence const& seq, Screen<Cell>& screen)
        {
            switch (seq.param(0))
            {
                case 5: screen.deviceStatusReport(); return ApplyResult::Ok;
                case 6: screen.reportCursorPosition(); return ApplyResult::Ok;
                default: return ApplyResult::Unsupported;
            }
        }

        template <typename Cell>
        ApplyResult DECRQPSR(sequence const& seq, Screen<Cell>& screen)
        {
            if (seq.parameterCount() != 1)
                return ApplyResult::Invalid; // -> error
            else if (seq.param(0) == 1)
                // TODO: https://vt100.net/docs/vt510-rm/DECCIR.html
                // TODO return emitCommand<RequestCursorState>(); // or call it with ...Detailed?
                return ApplyResult::Invalid;
            else if (seq.param(0) == 2)
            {
                screen.requestTabStops();
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        }

        template <typename Cell>
        ApplyResult DECSCUSR(sequence const& seq, Screen<Cell>& screen)
        {
            if (seq.parameterCount() <= 1)
            {
                switch (seq.param_or(0, sequence::parameter { 1 }))
                {
                    case 0:
                    case 1: screen.setCursorStyle(cursor_display::Blink, cursor_shape::Block); break;
                    case 2: screen.setCursorStyle(cursor_display::Steady, cursor_shape::Block); break;
                    case 3: screen.setCursorStyle(cursor_display::Blink, cursor_shape::Underscore); break;
                    case 4: screen.setCursorStyle(cursor_display::Steady, cursor_shape::Underscore); break;
                    case 5: screen.setCursorStyle(cursor_display::Blink, cursor_shape::Bar); break;
                    case 6: screen.setCursorStyle(cursor_display::Steady, cursor_shape::Bar); break;
                    default: return ApplyResult::Invalid;
                }
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        }

        template <typename Cell>
        ApplyResult EL(sequence const& seq, Screen<Cell>& screen)
        {
            switch (seq.param_or(0, sequence::parameter { 0 }))
            {
                case 0: screen.clearToEndOfLine(); break;
                case 1: screen.clearToBeginOfLine(); break;
                case 2: screen.clearLine(); break;
                default: return ApplyResult::Invalid;
            }
            return ApplyResult::Ok;
        }

        template <typename Cell>
        ApplyResult TBC(sequence const& seq, Screen<Cell>& screen)
        {
            if (seq.parameterCount() != 1)
            {
                screen.horizontalTabClear(HorizontalTabClear::UnderCursor);
                return ApplyResult::Ok;
            }

            switch (seq.param(0))
            {
                case 0: screen.horizontalTabClear(HorizontalTabClear::UnderCursor); break;
                case 3: screen.horizontalTabClear(HorizontalTabClear::AllTabs); break;
                default: return ApplyResult::Invalid;
            }
            return ApplyResult::Ok;
        }

        inline std::unordered_map<std::string_view, std::string_view> parseSubParamKeyValuePairs(
            std::string_view const& s)
        {
            return crispy::splitKeyValuePairs(s, ':');
        }

        template <typename Cell>
        ApplyResult setOrRequestDynamicColor(sequence const& seq,
                                             Screen<Cell>& screen,
                                             dynamic_color_name name)
        {
            auto const& value = seq.intermediateCharacters();
            if (value == "?")
                screen.requestDynamicColor(name);
            else if (auto color = parseColor(value); color.has_value())
                screen.setDynamicColor(name, color.value());
            else
                return ApplyResult::Invalid;

            return ApplyResult::Ok;
        }

        bool queryOrSetColorPalette(string_view text,
                                    std::function<void(uint8_t)> queryColor,
                                    std::function<void(uint8_t, rgb_color)> setColor)
        {
            // Sequence := [Param (';' Param)*]
            // Param    := Index ';' Query | Set
            // Index    := DIGIT+
            // Query    := ?'
            // Set      := 'rgb:' Hex8 '/' Hex8 '/' Hex8
            // Hex8     := [0-9A-Za-z] [0-9A-Za-z]
            // DIGIT    := [0-9]
            int index = -1;
            return crispy::split(text, ';', [&](string_view value) {
                if (index < 0)
                {
                    index = crispy::to_integer<10, int>(value).value_or(-1);
                    if (!(0 <= index && index <= 0xFF))
                        return false;
                }
                else if (value == "?"sv)
                {
                    queryColor((uint8_t) index);
                    index = -1;
                }
                else if (auto const color = parseColor(value))
                {
                    setColor((uint8_t) index, color.value());
                    index = -1;
                }
                else
                    return false;

                return true;
            });
        }

        template <typename Cell>
        ApplyResult RCOLPAL(sequence const& seq, Screen<Cell>& screen)
        {
            if (seq.intermediateCharacters().empty())
            {
                screen.colorPalette() = screen.defaultColorPalette();
                return ApplyResult::Ok;
            }

            auto const index = crispy::to_integer<10, uint8_t>(seq.intermediateCharacters());
            if (!index.has_value())
                return ApplyResult::Invalid;

            screen.colorPalette().palette[*index] = screen.defaultColorPalette().palette[*index];

            return ApplyResult::Ok;
        }

        ApplyResult SETCOLPAL(sequence const& seq, Terminal& terminal)
        {
            bool const ok = queryOrSetColorPalette(
                seq.intermediateCharacters(),
                [&](uint8_t index) {
                    auto const color = terminal.colorPalette().palette.at(index);
                    terminal.reply("\033]4;{};rgb:{:04x}/{:04x}/{:04x}\033\\",
                                   index,
                                   static_cast<uint16_t>(color.red) << 8 | color.red,
                                   static_cast<uint16_t>(color.green) << 8 | color.green,
                                   static_cast<uint16_t>(color.blue) << 8 | color.blue);
                },
                [&](uint8_t index, rgb_color color) { terminal.colorPalette().palette.at(index) = color; });

            return ok ? ApplyResult::Ok : ApplyResult::Invalid;
        }

        int toInt(string_view value)
        {
            int out = 0;
            for (auto const ch: value)
            {
                if (!(ch >= '0' && ch <= '9'))
                    return 0;

                out = out * 10 + (ch - '0');
            }
            return out;
        }

        ApplyResult setAllFont(sequence const& seq, Terminal& terminal)
        {
            // [read]  OSC 60 ST
            // [write] OSC 60 ; size ; regular ; bold ; italic ; bold italic ST
            auto const& params = seq.intermediateCharacters();
            auto const splits = crispy::split(params, ';');
            auto const param = [&](unsigned index) -> string_view {
                if (index < splits.size())
                    return splits.at(index);
                else
                    return {};
            };
            auto const emptyParams = [&]() -> bool {
                for (auto const& x: splits)
                    if (!x.empty())
                        return false;
                return true;
            }();
            if (emptyParams)
            {
                auto const fonts = terminal.getFontDef();
                terminal.reply("\033]60;{};{};{};{};{};{}\033\\",
                               int(fonts.size * 100), // precission-shift
                               fonts.regular,
                               fonts.bold,
                               fonts.italic,
                               fonts.boldItalic,
                               fonts.emoji);
            }
            else
            {
                auto const size = double(toInt(param(0))) / 100.0;
                auto const regular = string(param(1));
                auto const bold = string(param(2));
                auto const italic = string(param(3));
                auto const boldItalic = string(param(4));
                auto const emoji = string(param(5));
                terminal.setFontDef(font_def { size, regular, bold, italic, boldItalic, emoji });
            }
            return ApplyResult::Ok;
        }

        ApplyResult setFont(sequence const& seq, Terminal& terminal)
        {
            auto const& params = seq.intermediateCharacters();
            auto const splits = crispy::split(params, ';');

            if (splits.size() != 1)
                return ApplyResult::Invalid;

            if (splits[0] != "?"sv)
            {
                auto fontDef = font_def {};
                fontDef.regular = splits[0];
                terminal.setFontDef(fontDef);
            }
            else
            {
                auto const fonts = terminal.getFontDef();
                terminal.reply("\033]50;{}\033\\", fonts.regular);
            }

            return ApplyResult::Ok;
        }

        ApplyResult clipboard(sequence const& seq, Terminal& terminal)
        {
            // Only setting clipboard contents is supported, not reading.
            auto const& params = seq.intermediateCharacters();
            if (auto const splits = crispy::split(params, ';'); splits.size() == 2 && splits[0] == "c")
            {
                terminal.copyToClipboard(crispy::base64::decode(splits[1]));
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        }

        template <typename Cell>
        ApplyResult NOTIFY(sequence const& seq, Screen<Cell>& screen)
        {
            auto const& value = seq.intermediateCharacters();
            if (auto const splits = crispy::split(value, ';'); splits.size() == 3 && splits[0] == "notify")
            {
                screen.notify(string(splits[1]), string(splits[2]));
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Unsupported;
        }

        template <typename Cell>
        ApplyResult SETCWD(sequence const& seq, Screen<Cell>& screen)
        {
            string const& url = seq.intermediateCharacters();
            screen.setCurrentWorkingDirectory(url);
            return ApplyResult::Ok;
        }

        ApplyResult CAPTURE(sequence const& seq, Terminal& terminal)
        {
            // CSI Mode ; [; Count] t
            //
            // Mode: 0 = physical lines
            //       1 = logical lines (unwrapped)
            //
            // Count: number of lines to capture from main page aera's bottom upwards
            //        If omitted or 0, the main page area's line count will be used.

            auto const logicalLines = seq.param_or(0, 0);
            if (logicalLines != 0 && logicalLines != 1)
                return ApplyResult::Invalid;

            auto const lineCount = LineCount(seq.param_or(1, *terminal.pageSize().lines));

            terminal.requestCaptureBuffer(lineCount, logicalLines);

            return ApplyResult::Ok;
        }

        template <typename Cell>
        ApplyResult HYPERLINK(sequence const& seq, Screen<Cell>& screen)
        {
            auto const& value = seq.intermediateCharacters();
            // hyperlink_OSC ::= OSC '8' ';' params ';' URI
            // params := pair (':' pair)*
            // pair := TEXT '=' TEXT
            if (auto const pos = value.find(';'); pos != sequence::intermediaries::npos)
            {
                auto const paramsStr = value.substr(0, pos);
                auto const params = parseSubParamKeyValuePairs(paramsStr);

                auto id = string {};
                if (auto const p = params.find("id"); p != params.end())
                    id = p->second;

                if (pos + 1 != value.size())
                    screen.hyperlink(id, value.substr(pos + 1));
                else
                    screen.hyperlink(string { id }, string {});

                return ApplyResult::Ok;
            }
            else
                screen.hyperlink(string {}, string {});

            return ApplyResult::Ok;
        }

        template <typename Cell>
        ApplyResult saveDECModes(sequence const& seq, Screen<Cell>& screen)
        {
            vector<dec_mode> modes;
            for (size_t i = 0; i < seq.parameterCount(); ++i)
                if (optional<dec_mode> mode = toDECMode(seq.param(i)); mode.has_value())
                    modes.push_back(mode.value());
            screen.saveModes(modes);
            return ApplyResult::Ok;
        }

        template <typename Cell>
        ApplyResult restoreDECModes(sequence const& seq, Screen<Cell>& screen)
        {
            vector<dec_mode> modes;
            for (size_t i = 0; i < seq.parameterCount(); ++i)
                if (optional<dec_mode> mode = toDECMode(seq.param(i)); mode.has_value())
                    modes.push_back(mode.value());
            screen.restoreModes(modes);
            return ApplyResult::Ok;
        }

        ApplyResult WINDOWMANIP(sequence const& seq, Terminal& terminal)
        {
            if (seq.parameterCount() == 3)
            {
                switch (seq.param(0))
                {
                    case 4: // resize in pixel units
                        terminal.requestWindowResize(
                            image_size { width(seq.param(2)), height(seq.param(1)) });
                        break;
                    case 8: // resize in cell units
                        terminal.requestWindowResize(PageSize { LineCount::cast_from(seq.param(1)),
                                                                ColumnCount::cast_from(seq.param(2)) });
                        break;
                    case 22: terminal.saveWindowTitle(); break;
                    case 23: terminal.restoreWindowTitle(); break;
                    default: return ApplyResult::Unsupported;
                }
                return ApplyResult::Ok;
            }
            else if (seq.parameterCount() == 2 || seq.parameterCount() == 1)
            {
                switch (seq.param(0))
                {
                    case 4:
                    case 8:
                        // this means, resize to full display size
                        // TODO: just create a dedicated callback for fulscreen resize!
                        terminal.requestWindowResize(image_size {});
                        break;
                    case 14:
                        if (seq.parameterCount() == 2 && seq.param(1) == 2)
                            terminal.primaryScreen().requestPixelSize(
                                RequestPixelSize::WindowArea); // CSI 14 ; 2 t
                        else
                            terminal.primaryScreen().requestPixelSize(RequestPixelSize::TextArea); // CSI 14 t
                        break;
                    case 16: terminal.primaryScreen().requestPixelSize(RequestPixelSize::CellArea); break;
                    case 18: terminal.primaryScreen().requestCharacterSize(RequestPixelSize::TextArea); break;
                    case 19:
                        terminal.primaryScreen().requestCharacterSize(RequestPixelSize::WindowArea);
                        break;
                    case 22: {
                        switch (seq.param(1))
                        {
                            case 0:
                                terminal.saveWindowTitle();
                                break; // CSI 22 ; 0 t | save icon & window title
                            case 1: return ApplyResult::Unsupported;   // CSI 22 ; 1 t | save icon title
                            case 2: terminal.saveWindowTitle(); break; // CSI 22 ; 2 t | save window title
                            default: return ApplyResult::Unsupported;
                        }
                        return ApplyResult::Ok;
                    }
                    case 23: {
                        switch (seq.param(1))
                        {
                            case 0:
                                terminal.restoreWindowTitle();
                                break; // CSI 22 ; 0 t | save icon & window title
                            case 1: return ApplyResult::Unsupported;      // CSI 22 ; 1 t | save icon title
                            case 2: terminal.restoreWindowTitle(); break; // CSI 22 ; 2 t | save window title
                            default: return ApplyResult::Unsupported;
                        }
                        return ApplyResult::Ok;
                    }
                }
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Unsupported;
        }

        template <typename Cell>
        ApplyResult XTSMGRAPHICS(sequence const& seq, Screen<Cell>& screen)
        {
            auto const Pi = seq.param<unsigned>(0);
            auto const Pa = seq.param<unsigned>(1);
            auto const Pv = seq.param_or<unsigned>(2, 0);
            auto const Pu = seq.param_or<unsigned>(3, 0);

            auto const item = [&]() -> optional<XtSmGraphics::Item> {
                switch (Pi)
                {
                    case 1: return XtSmGraphics::Item::NumberOfColorRegisters;
                    case 2: return XtSmGraphics::Item::SixelGraphicsGeometry;
                    case 3: return XtSmGraphics::Item::ReGISGraphicsGeometry;
                    default: return nullopt;
                }
            }();
            if (!item.has_value())
                return ApplyResult::Invalid;

            auto const action = [&]() -> optional<XtSmGraphics::Action> {
                switch (Pa)
                {
                    case 1: return XtSmGraphics::Action::Read;
                    case 2: return XtSmGraphics::Action::ResetToDefault;
                    case 3: return XtSmGraphics::Action::SetToValue;
                    case 4: return XtSmGraphics::Action::ReadLimit;
                    default: return nullopt;
                }
            }();
            if (!action.has_value())
                return ApplyResult::Invalid;

            if (*item != XtSmGraphics::Item::NumberOfColorRegisters
                && *action == XtSmGraphics::Action::SetToValue && (!Pv || !Pu))
                return ApplyResult::Invalid;

            auto const value = [&]() -> XtSmGraphics::Value {
                using Action = XtSmGraphics::Action;
                switch (*action)
                {
                    case Action::Read:
                    case Action::ResetToDefault:
                    case Action::ReadLimit: return std::monostate {};
                    case Action::SetToValue:
                        return *item == XtSmGraphics::Item::NumberOfColorRegisters
                                   ? XtSmGraphics::Value { Pv }
                                   : XtSmGraphics::Value { image_size { width(Pv), height(Pu) } };
                }
                return std::monostate {};
            }();

            screen.smGraphics(*item, *action, value);

            return ApplyResult::Ok;
        }
    } // namespace
} // namespace impl
// }}}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::executeControlCode(char controlCode)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTTraceSequenceLog)
        VTTraceSequenceLog()(
            "control U+{:02X} ({})", controlCode, to_string(static_cast<ControlCode::c0>(controlCode)));
#endif

    _terminal.state().instructionCounter++;
    switch (controlCode)
    {
        case 0x00: // NUL
            break;
        case 0x07: // BEL
            _terminal.bell();
            break;
        case 0x08: // BS
            backspace();
            break;
        case 0x09: // TAB
            moveCursorToNextTab();
            break;
        case 0x0A: // LF
            linefeed();
            break;
        case 0x0B: // VT
            // Even though VT means Vertical Tab, it seems that xterm is doing an IND instead.
            [[fallthrough]];
        case 0x0C: // FF
            // Even though FF means Form Feed, it seems that xterm is doing an IND instead.
            index();
            break;
        case LS1.finalSymbol: // LS1 (SO)
            // Invokes G1 character set into GL. G1 is designated by a select-character-set (SCS) sequence.
            _cursor.charsets.lockingShift(charset_table::G1);
            break;
        case LS0.finalSymbol: // LSO (SI)
            // Invoke G0 character set into GL. G0 is designated by a select-character-set sequence (SCS).
            _cursor.charsets.lockingShift(charset_table::G0);
            break;
        case 0x0D: moveCursorToBeginOfLine(); break;
        case 0x37: saveCursor(); break;
        case 0x38: restoreCursor(); break;
        default:
            // if (VTParserLog)
            //     VTParserLog()("Unsupported C0 sequence: {}", crispy::escape((uint8_t) controlCode));
            break;
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::saveCursor()
{
    // https://vt100.net/docs/vt510-rm/DECSC.html
    _savedCursor = _cursor;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::restoreCursor()
{
    // https://vt100.net/docs/vt510-rm/DECRC.html
    restoreCursor(_savedCursor);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::restoreCursor(Cursor const& savedCursor)
{
    _cursor = savedCursor;
    _cursor.position = clampCoordinate(_cursor.position);
    _terminal.setMode(dec_mode::AutoWrap, savedCursor.autoWrap);
    _terminal.setMode(dec_mode::Origin, savedCursor.originMode);
    updateCursorIterator();
    verifyState();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::processSequence(sequence const& seq)
{
#if defined(LIBTERMINAL_LOG_TRACE)
    if (VTTraceSequenceLog)
        VTTraceSequenceLog()("Handle VT sequence: {}", seq);
#endif

    // std::cerr << fmt::format("\t{} \t; {}\n", seq,
    //         seq.functionDefinition() ? seq.functionDefinition()->comment : ""sv);

    _terminal.state().instructionCounter++;
    if (function_definition const* funcSpec = seq.functionDefinition(); funcSpec != nullptr)
        applyAndLog(*funcSpec, seq);
    else if (VTParserLog)
        VTParserLog()("Unknown VT sequence: {}", seq);
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Screen<Cell>::applyAndLog(function_definition const& function, sequence const& seq)
{
    auto const result = apply(function, seq);
    switch (result)
    {
        case ApplyResult::Invalid: {
            VTParserLog()("Invalid VT sequence: {}", seq);
            break;
        }
        case ApplyResult::Unsupported: {
            VTParserLog()("Unsupported VT sequence: {}", seq);
            break;
        }
        case ApplyResult::Ok: {
            _terminal.verifyState();
            break;
        }
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
ApplyResult Screen<Cell>::apply(function_definition const& function, sequence const& seq)
{
    // This function assumed that the incoming instruction has been already resolved to a given
    // FunctionDefinition
    switch (function)
    {
        // C0
        case BEL: _terminal.bell(); break;
        case BS: backspace(); break;
        case TAB: moveCursorToNextTab(); break;
        case LF: linefeed(); break;
        case VT: [[fallthrough]];
        case FF: index(); break;
        case CR: moveCursorToBeginOfLine(); break;

        // ESC
        case SCS_G0_SPECIAL: designateCharset(charset_table::G0, charset_id::Special); break;
        case SCS_G0_USASCII: designateCharset(charset_table::G0, charset_id::USASCII); break;
        case SCS_G1_SPECIAL: designateCharset(charset_table::G1, charset_id::Special); break;
        case SCS_G1_USASCII: designateCharset(charset_table::G1, charset_id::USASCII); break;
        case DECALN: screenAlignmentPattern(); break;
        case DECBI: backIndex(); break;
        case DECFI: forwardIndex(); break;
        case DECKPAM: applicationKeypadMode(true); break;
        case DECKPNM: applicationKeypadMode(false); break;
        case DECRS: restoreCursor(); break;
        case DECSC: saveCursor(); break;
        case HTS: horizontalTabSet(); break;
        case IND: index(); break;
        case NEL: moveCursorToNextLine(LineCount(1)); break;
        case RI: reverseIndex(); break;
        case RIS: _terminal.hardReset(); break;
        case SS2: singleShiftSelect(charset_table::G2); break;
        case SS3: singleShiftSelect(charset_table::G3); break;

        // CSI
        case ANSISYSSC: restoreCursor(); break;
        case CBT:
            cursorBackwardTab(tab_stop_count::cast_from(seq.param_or(0, sequence::parameter { 1 })));
            break;
        case CHA: moveCursorToColumn(seq.param_or<column_offset>(0, column_offset { 1 }) - 1); break;
        case CHT:
            cursorForwardTab(tab_stop_count::cast_from(seq.param_or(0, sequence::parameter { 1 })));
            break;
        case CNL:
            moveCursorToNextLine(LineCount::cast_from(seq.param_or(0, sequence::parameter { 1 })));
            break;
        case CPL:
            moveCursorToPrevLine(LineCount::cast_from(seq.param_or(0, sequence::parameter { 1 })));
            break;
        case CPR: return impl::CPR(seq, *this);
        case CUB: moveCursorBackward(seq.param_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case CUD: moveCursorDown(seq.param_or<LineCount>(0, LineCount { 1 })); break;
        case CUF: moveCursorForward(seq.param_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case CUP:
            moveCursorTo(line_offset::cast_from(seq.param_or<int>(0, 1) - 1),
                         column_offset::cast_from(seq.param_or<int>(1, 1) - 1));
            break;
        case CUU: moveCursorUp(seq.param_or<LineCount>(0, LineCount { 1 })); break;
        case DA1: sendDeviceAttributes(); break;
        case DA2: sendTerminalId(); break;
        case DA3:
            // terminal identification, 4 hex codes
            _terminal.reply("\033P!|C0000000\033\\");
            break;
        case DCH: deleteCharacters(seq.param_or<ColumnCount>(0, ColumnCount { 1 })); break;
        case DECCARA: {
            auto const origin = this->origin();
            auto const top = line_offset(seq.param_or(0, *origin.line + 1) - 1);
            auto const left = column_offset(seq.param_or(1, *origin.column + 1) - 1);
            auto const bottom = line_offset(seq.param_or(2, *pageSize().lines) - 1);
            auto const right = column_offset(seq.param_or(3, *pageSize().columns) - 1);
            for (auto row = top; row <= bottom; ++row)
            {
                for (auto column = left; column <= right; ++column)
                {
                    auto& cell = at(row, column);
                    impl::applySGR(cell, seq, 4, seq.parameterCount());
                    // Maybe move setGraphicsRendition to Screen::cursor() ?
                }
            }
        }
        break;
        case DECCRA: {
            // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
            // DECCRA is not affected by the page margins.
            auto const origin = this->origin();
            auto const topValue = top(seq.param_or(0, *origin.line + 1) - 1);
            auto const leftValue = left(seq.param_or(1, *origin.column + 1) - 1);
            auto const bottomValue = bottom(seq.param_or(2, *pageSize().lines) - 1);
            auto const rightValue = right(seq.param_or(3, *pageSize().columns) - 1);
            auto const page = seq.param_or(4, 0);

            auto const targetTop = line_offset(seq.param_or(5, *origin.line + 1) - 1);
            auto const targetLeft = column_offset(seq.param_or(6, *origin.column + 1) - 1);
            auto const targetTopLeft = cell_location { targetTop, targetLeft };
            auto const targetPage = seq.param_or(7, 0);

            copyArea(rect { topValue, leftValue, bottomValue, rightValue }, page, targetTopLeft, targetPage);
        }
        break;
        case DECERA: {
            // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
            auto const origin = this->origin();
            auto const top = seq.param_or(0, *origin.line + 1) - 1;
            auto const left = seq.param_or(1, *origin.column + 1) - 1;

            // If the value of Pt, Pl, Pb, or Pr exceeds the width or height of the active page, then the
            // value is treated as the width or height of that page.
            auto const size = pageSize();
            auto const bottom = min(seq.param_or(2, unbox<int>(size.lines)), unbox<int>(size.lines)) - 1;
            auto const right = min(seq.param_or(3, unbox<int>(size.columns)), unbox<int>(size.columns)) - 1;

            eraseArea(top, left, bottom, right);
        }
        break;
        case DECFRA: {
            auto const ch = seq.param_or(0, sequence::parameter { 0 });
            // The coordinates of the rectangular area are affected by the setting of origin mode (DECOM).
            auto const origin = this->origin();
            auto const top = seq.param_or(0, origin.line);
            auto const left = seq.param_or(1, origin.column);

            // If the value of Pt, Pl, Pb, or Pr exceeds the width or height of the active page, then the
            // value is treated as the width or height of that page.
            auto const size = pageSize();
            auto const bottom = min(seq.param_or(2, *size.lines), *size.lines);
            auto const right = min(seq.param_or(3, *size.columns), *size.columns);

            fillArea(ch, *top, *left, bottom, right);
        }
        break;
        case DECDC: deleteColumns(seq.param_or(0, ColumnCount(1))); break;
        case DECIC: insertColumns(seq.param_or(0, ColumnCount(1))); break;
        case DECSCA: {
            auto const Pc = seq.param_or(0, 0);
            switch (Pc)
            {
                case 1:
                    _cursor.graphicsRendition.flags |= cell_flags::CharacterProtected;
                    return ApplyResult::Ok;
                case 0:
                case 2:
                    _cursor.graphicsRendition.flags &= ~cell_flags::CharacterProtected;
                    return ApplyResult::Ok;
                default: return ApplyResult::Invalid;
            }
        }
        case DECSED: {
            switch (seq.param_or(0, sequence::parameter { 0 }))
            {
                case 0: selectiveEraseToEndOfScreen(); break;
                case 1: selectiveEraseToBeginOfScreen(); break;
                case 2: selectiveEraseScreen(); break;
                default: return ApplyResult::Unsupported;
            }
            return ApplyResult::Ok;
        }
        case DECSERA: {
            auto const topValue = seq.param_or(0, top(1)) - 1;
            auto const leftValue = seq.param_or(1, left(1)) - 1;
            auto const bottom = seq.param_or(2, bottom::cast_from(_settings.pageSize.lines)) - 1;
            auto const right = seq.param_or(3, right::cast_from(_settings.pageSize.columns)) - 1;
            selectiveEraseArea(rect { topValue, leftValue, bottom, right });
            return ApplyResult::Ok;
        }
        case DECSEL: {
            switch (seq.param_or(0, sequence::parameter { 0 }))
            {
                case 0: selectiveEraseToEndOfLine(); break;
                case 1: selectiveEraseToBeginOfLine(); break;
                case 2: selectiveEraseLine(_cursor.position.line); break;
                default: return ApplyResult::Invalid;
            }
            return ApplyResult::Ok;
        }
        case DECRM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setModeDEC(seq, i, false, _terminal);
                r = max(r, t);
            });
            return r;
        }
        break;
        case DECRQM:
            if (seq.parameterCount() != 1)
                return ApplyResult::Invalid;
            requestDECMode(seq.param(0));
            return ApplyResult::Ok;
        case DECRQM_ANSI:
            if (seq.parameterCount() != 1)
                return ApplyResult::Invalid;
            requestAnsiMode(seq.param(0));
            return ApplyResult::Ok;
        case DECRQPSR: return impl::DECRQPSR(seq, *this);
        case DECSCUSR: return impl::DECSCUSR(seq, *this);
        case DECSCPP:
            if (auto const columnCount = seq.param_or(0, 80); columnCount == 80 || columnCount == 132)
            {
                // EXTENSION: only 80 and 132 are specced, but we allow any.
                _terminal.resizeColumns(ColumnCount(columnCount), false);
                return ApplyResult::Ok;
            }
            else
                return ApplyResult::Invalid;
        case DECSNLS:
            _terminal.resizeScreen(PageSize { pageSize().lines, seq.param<ColumnCount>(0) });
            return ApplyResult::Ok;
        case DECSLRM: {
            auto l = decr(seq.param_opt<column_offset>(0));
            auto r = decr(seq.param_opt<column_offset>(1));
            _terminal.setLeftRightMargin(l, r);
            moveCursorTo({}, {});
        }
        break;
        case DECSM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setModeDEC(seq, i, true, _terminal);
                r = max(r, t);
            });
            return r;
        }
        case DECSTBM:
            _terminal.setTopBottomMargin(decr(seq.param_opt<line_offset>(0)),
                                         decr(seq.param_opt<line_offset>(1)));
            moveCursorTo({}, {});
            break;
        case DECSTR:
            // For VTType VT100 and VT52 ignore this sequence
            if (_terminal.state().terminalId == vt_type::VT100)
                return ApplyResult::Invalid;
            _terminal.softReset();
            break;
        case DECXCPR: reportExtendedCursorPosition(); break;
        case DL: deleteLines(seq.param_or(0, LineCount(1))); break;
        case ECH: eraseCharacters(seq.param_or(0, ColumnCount(1))); break;
        case ED:
            if (seq.parameterCount() == 0)
                clearToEndOfScreen();
            else
            {
                for (size_t i = 0; i < seq.parameterCount(); ++i)
                {
                    switch (seq.param(i))
                    {
                        case 0: clearToEndOfScreen(); break;
                        case 1: clearToBeginOfScreen(); break;
                        case 2: clearScreen(); break;
                        case 3:
                            _grid.clearHistory();
                            _terminal.scrollbackBufferCleared();
                            break;
                    }
                }
            }
            break;
        case EL: return impl::EL(seq, *this);
        case HPA: moveCursorToColumn(seq.param<column_offset>(0) - 1); break;
        case HPR: moveCursorForward(seq.param<ColumnCount>(0)); break;
        case HVP:
            moveCursorTo(seq.param_or(0, line_offset(1)) - 1, seq.param_or(1, column_offset(1)) - 1);
            break; // YES, it's like a CUP!
        case ICH: insertCharacters(seq.param_or(0, ColumnCount { 1 })); break;
        case IL: insertLines(seq.param_or(0, LineCount { 1 })); break;
        case REP:
            if (precedingGraphicCharacter())
            {
                auto const requestedCount = seq.param<size_t>(0);
                auto const availableColumns =
                    (getMargin().hori.to - cursor().position.column).template as<size_t>();
                auto const effectiveCount = min(requestedCount, availableColumns);
                for (size_t i = 0; i < effectiveCount; i++)
                    writeText(precedingGraphicCharacter());
            }
            break;
        case RM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setAnsiMode(seq, i, false, _terminal);
                r = max(r, t);
            });
            return r;
        }
        break;
        case SCOSC: saveCursor(); break;
        case SD: scrollDown(seq.param_or<LineCount>(0, LineCount { 1 })); break;
        case SETMARK: setMark(); break;
        case SGR: return impl::applySGR(_terminal, seq, 0, seq.parameterCount());
        case SM: {
            ApplyResult r = ApplyResult::Ok;
            crispy::for_each(crispy::times(seq.parameterCount()), [&](size_t i) {
                auto const t = impl::setAnsiMode(seq, i, true, _terminal);
                r = max(r, t);
            });
            return r;
        }
        case SU: scrollUp(seq.param_or<LineCount>(0, LineCount(1))); break;
        case TBC: return impl::TBC(seq, *this);
        case VPA: moveCursorToLine(seq.param_or<line_offset>(0, line_offset { 1 }) - 1); break;
        case WINMANIP: return impl::WINDOWMANIP(seq, _terminal);
        case XTRESTORE: return impl::restoreDECModes(seq, *this);
        case XTSAVE: return impl::saveDECModes(seq, *this);
        case XTPOPCOLORS:
            if (!seq.parameterCount())
                _terminal.popColorPalette(0);
            else
                for (size_t i = 0; i < seq.parameterCount(); ++i)
                    _terminal.popColorPalette(seq.param<size_t>(i));
            return ApplyResult::Ok;
        case XTPUSHCOLORS:
            if (!seq.parameterCount())
                _terminal.pushColorPalette(0);
            else
                for (size_t i = 0; i < seq.parameterCount(); ++i)
                    _terminal.pushColorPalette(seq.param<size_t>(i));
            return ApplyResult::Ok;
        case XTREPORTCOLORS: _terminal.reportColorPaletteStack(); return ApplyResult::Ok;
        case XTSMGRAPHICS: return impl::XTSMGRAPHICS(seq, *this);
        case XTVERSION:
            _terminal.reply(fmt::format("\033P>|{} {}\033\\", LIBTERMINAL_NAME, LIBTERMINAL_VERSION_STRING));
            return ApplyResult::Ok;
        case DECSSDT: {
            // Changes the status line display type.
            switch (seq.param_or(0, 0))
            {
                case 0: _terminal.setStatusDisplay(status_display_type::None); break;
                case 1: _terminal.setStatusDisplay(status_display_type::Indicator); break;
                case 2:
                    if (_terminal.statusDisplayType() != status_display_type::HostWritable)
                        _terminal.requestShowHostWritableStatusLine();
                    break;
                default: return ApplyResult::Invalid;
            }
            break;
        }
        case DECSASD:
            // Selects whether the terminal sends data to the main display or the status line.
            switch (seq.param_or(0, 0))
            {
                case 0:
                    if (_state.activeStatusDisplay == active_status_display::StatusLine
                        && _state.syncWindowTitleWithHostWritableStatusDisplay)
                    {
                        _terminal.setWindowTitle(crispy::trimRight(
                            _terminal.hostWritableStatusLineDisplay().grid().lineText(line_offset(0))));
                        _state.syncWindowTitleWithHostWritableStatusDisplay = false;
                    }
                    _terminal.setActiveStatusDisplay(active_status_display::Main);
                    break;

                case 1: _terminal.setActiveStatusDisplay(active_status_display::StatusLine); break;
                default: return ApplyResult::Invalid;
            }
            break;

        case DECPS: _terminal.playSound(seq.getParameters()); break;
        // OSC
        case SETTITLE:
            //(not supported) ChangeIconTitle(seq.intermediateCharacters());
            _terminal.setWindowTitle(seq.intermediateCharacters());
            return ApplyResult::Ok;
        case SETICON: return ApplyResult::Ok; // NB: Silently ignore!
        case SETWINTITLE: _terminal.setWindowTitle(seq.intermediateCharacters()); break;
        case SETXPROP: return ApplyResult::Unsupported;
        case SETCOLPAL: return impl::SETCOLPAL(seq, _terminal);
        case RCOLPAL: return impl::RCOLPAL(seq, *this);
        case SETCWD: return impl::SETCWD(seq, *this);
        case HYPERLINK: return impl::HYPERLINK(seq, *this);
        case XTCAPTURE: return impl::CAPTURE(seq, _terminal);
        case COLORFG:
            return impl::setOrRequestDynamicColor(seq, *this, dynamic_color_name::DefaultForegroundColor);
        case COLORBG:
            return impl::setOrRequestDynamicColor(seq, *this, dynamic_color_name::DefaultBackgroundColor);
        case COLORCURSOR:
            return impl::setOrRequestDynamicColor(seq, *this, dynamic_color_name::TextCursorColor);
        case COLORMOUSEFG:
            return impl::setOrRequestDynamicColor(seq, *this, dynamic_color_name::MouseForegroundColor);
        case COLORMOUSEBG:
            return impl::setOrRequestDynamicColor(seq, *this, dynamic_color_name::MouseBackgroundColor);
        case SETFONT: return impl::setFont(seq, _terminal);
        case SETFONTALL: return impl::setAllFont(seq, _terminal);
        case CLIPBOARD: return impl::clipboard(seq, _terminal);
        // TODO: case COLORSPECIAL: return impl::setOrRequestDynamicColor(seq, _output,
        // DynamicColorName::HighlightForegroundColor);
        case RCOLORFG: resetDynamicColor(dynamic_color_name::DefaultForegroundColor); break;
        case RCOLORBG: resetDynamicColor(dynamic_color_name::DefaultBackgroundColor); break;
        case RCOLORCURSOR: resetDynamicColor(dynamic_color_name::TextCursorColor); break;
        case RCOLORMOUSEFG: resetDynamicColor(dynamic_color_name::MouseForegroundColor); break;
        case RCOLORMOUSEBG: resetDynamicColor(dynamic_color_name::MouseBackgroundColor); break;
        case RCOLORHIGHLIGHTFG: resetDynamicColor(dynamic_color_name::HighlightForegroundColor); break;
        case RCOLORHIGHLIGHTBG: resetDynamicColor(dynamic_color_name::HighlightBackgroundColor); break;
        case NOTIFY: return impl::NOTIFY(seq, *this);
        case DUMPSTATE: inspect(); break;

        // hooks
        case DECSIXEL: _state.sequencer.hookParser(hookSixel(seq)); break;
        case STP: _state.sequencer.hookParser(hookSTP(seq)); break;
        case DECRQSS: _state.sequencer.hookParser(hookDECRQSS(seq)); break;
        case XTGETTCAP: _state.sequencer.hookParser(hookXTGETTCAP(seq)); break;

        default: return ApplyResult::Unsupported;
    }
    return ApplyResult::Ok;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
unique_ptr<ParserExtension> Screen<Cell>::hookSixel(sequence const& seq)
{
    auto const Pa = seq.param_or(0, 1);
    auto const Pb = seq.param_or(1, 2);

    auto const aspectVertical = [](int pA) {
        switch (pA)
        {
            case 9:
            case 8:
            case 7: return 1;
            case 6:
            case 5: return 2;
            case 4:
            case 3: return 3;
            case 2: return 5;
            case 1:
            case 0: return 2;
            default: return 1;
        }
    }(Pa);

    auto const aspectHorizontal = 1;
    auto const transparentBackground = Pb == 1;

    _sixelImageBuilder = make_unique<sixel_image_builder>(
        _terminal.state().effectiveImageCanvasSize,
        aspectVertical,
        aspectHorizontal,
        transparentBackground ? rgba_color { 0, 0, 0, 0 } : _terminal.state().colorPalette.defaultBackground,
        _terminal.state().usePrivateColorRegisters
            ? make_shared<sixel_color_palette>(_terminal.state().maxImageRegisterCount,
                                               clamp(_terminal.state().maxImageRegisterCount, 0u, 16384u))
            : _terminal.state().imageColorPalette);

    return make_unique<sixel_parser>(*_sixelImageBuilder, [this]() {
        {
            sixelImage(_sixelImageBuilder->size(), std::move(_sixelImageBuilder->data()));
        }
    });
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
unique_ptr<ParserExtension> Screen<Cell>::hookSTP(sequence const& /*seq*/)
{
    return make_unique<SimpleStringCollector>(
        [this](string_view const& data) { _terminal.setTerminalProfile(unicode::convert_to<char>(data)); });
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
unique_ptr<ParserExtension> Screen<Cell>::hookXTGETTCAP(sequence const& /*seq*/)
{
    // DCS + q Pt ST
    //           Request Termcap/Terminfo String (XTGETTCAP), xterm.  The
    //           string following the "q" is a list of names encoded in
    //           hexadecimal (2 digits per character) separated by ; which
    //           correspond to termcap or terminfo key names.
    //           A few special features are also recognized, which are not key
    //           names:
    //
    //           o   Co for termcap colors (or colors for terminfo colors), and
    //
    //           o   TN for termcap name (or name for terminfo name).
    //
    //           o   RGB for the ncurses direct-color extension.
    //               Only a terminfo name is provided, since termcap
    //               applications cannot use this information.
    //
    //           xterm responds with
    //           DCS 1 + r Pt ST for valid requests, adding to Pt an = , and
    //           the value of the corresponding string that xterm would send,
    //           or
    //           DCS 0 + r Pt ST for invalid requests.
    //           The strings are encoded in hexadecimal (2 digits per
    //           character).

    return make_unique<SimpleStringCollector>([this](string_view const& data) {
        auto const capsInHex = crispy::split(data, ';');
        for (auto hexCap: capsInHex)
        {
            auto const hexCap8 = unicode::convert_to<char>(hexCap);
            if (auto const capOpt = crispy::fromHexString(string_view(hexCap8.data(), hexCap8.size())))
                requestCapability(capOpt.value());
        }
    });
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
unique_ptr<ParserExtension> Screen<Cell>::hookDECRQSS(sequence const& /*seq*/)
{
    return make_unique<SimpleStringCollector>([this](string_view data) {
        auto const s = [](string_view dataString) -> optional<RequestStatusString> {
            auto const mappings = array<pair<string_view, RequestStatusString>, 11> {
                pair { "m", RequestStatusString::SGR },       pair { "\"p", RequestStatusString::DECSCL },
                pair { " q", RequestStatusString::DECSCUSR }, pair { "\"q", RequestStatusString::DECSCA },
                pair { "r", RequestStatusString::DECSTBM },   pair { "s", RequestStatusString::DECSLRM },
                pair { "t", RequestStatusString::DECSLPP },   pair { "$|", RequestStatusString::DECSCPP },
                pair { "$}", RequestStatusString::DECSASD },  pair { "$~", RequestStatusString::DECSSDT },
                pair { "*|", RequestStatusString::DECSNLS }
            };
            for (auto const& mapping: mappings)
                if (dataString == mapping.first)
                    return mapping.second;
            return nullopt;
        }(data);

        if (s.has_value())
            requestStatusString(s.value());

        // TODO: handle batching
    });
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
optional<cell_location> Screen<Cell>::search(std::u32string_view searchText, cell_location startPosition)
{
    // TODO use LogicalLines to spawn logical lines for improving the search on wrapped lines.

    if (searchText.empty())
        return nullopt;

    // First try match at start location.
    if (_grid.lineAt(startPosition.line).matchTextAt(searchText, startPosition.column))
        return startPosition;

    // Search reverse until found or exhausted.
    auto lines = _grid.logicalLinesFrom(startPosition.line);
    for (auto& line: lines)
    {
        auto result = line.search(searchText, startPosition.column);
        if (result.has_value())
            return result; // new match found
        startPosition.column = column_offset(0);
    }
    return nullopt;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
optional<cell_location> Screen<Cell>::searchReverse(std::u32string_view searchText,
                                                    cell_location startPosition)
{
    // TODO use LogicalLinesReverse to spawn logical lines for improving the search on wrapped lines.

    if (searchText.empty())
        return nullopt;

    // First try match at start location.
    if (_grid.lineAt(startPosition.line).matchTextAt(searchText, startPosition.column))
        return startPosition;

    // Search reverse until found or exhausted.
    auto lines = _grid.logicalLinesReverseFrom(startPosition.line);
    for (auto const& line: lines)
    {
        auto result = line.searchReverse(searchText, startPosition.column);
        if (result.has_value())
            return result; // new match found
        startPosition.column = boxed_cast<column_offset>(pageSize().columns) - 1;
    }
    return nullopt;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
bool Screen<Cell>::isCursorInsideMargins() const noexcept
{
    bool const insideVerticalMargin = getMargin().vert.contains(_cursor.position.line);
    bool const insideHorizontalMargin = !_terminal.isModeEnabled(dec_mode::LeftRightMargin)
                                        || getMargin().hori.contains(_cursor.position.column);
    return insideVerticalMargin && insideHorizontalMargin;
}

} // namespace terminal

#include <vtbackend/cell/CompactCell.h>
template class terminal::Screen<terminal::compact_cell>;

#include <vtbackend/cell/SimpleCell.h>
template class terminal::Screen<terminal::simple_cell>;
