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

#include <terminal/Cell.h>
#include <terminal/Charset.h>
#include <terminal/ColorPalette.h>
#include <terminal/GraphicsAttributes.h>
#include <terminal/Grid.h>
#include <terminal/Hyperlink.h>
#include <terminal/InputGenerator.h>
#include <terminal/InputHandler.h>
#include <terminal/Parser.h>
#include <terminal/ScreenEvents.h> // ScreenType
#include <terminal/Sequencer.h>
#include <terminal/ViCommands.h>
#include <terminal/ViInputHandler.h>
#include <terminal/primitives.h>

#include <unicode/utf8.h>

#include <fmt/format.h>

#include <bitset>
#include <functional>
#include <memory>
#include <stack>
#include <vector>

namespace terminal
{

class Terminal;

// {{{ Modes
/// API for setting/querying terminal modes.
///
/// This abstracts away the actual implementation for more intuitive use and easier future adaptability.
class Modes
{
  public:
    void set(AnsiMode _mode, bool _enabled) { ansi_.set(static_cast<size_t>(_mode), _enabled); }

    void set(DECMode _mode, bool _enabled) { dec_.set(static_cast<size_t>(_mode), _enabled); }

    [[nodiscard]] bool enabled(AnsiMode _mode) const noexcept
    {
        return ansi_.test(static_cast<size_t>(_mode));
    }

    [[nodiscard]] bool enabled(DECMode _mode) const noexcept { return dec_.test(static_cast<size_t>(_mode)); }

    void save(std::vector<DECMode> const& _modes)
    {
        for (DECMode const mode: _modes)
            savedModes_[mode].push_back(enabled(mode));
    }

    void restore(std::vector<DECMode> const& _modes)
    {
        for (DECMode const mode: _modes)
        {
            if (auto i = savedModes_.find(mode); i != savedModes_.end() && !i->second.empty())
            {
                auto& saved = i->second;
                set(mode, saved.back());
                saved.pop_back();
            }
        }
    }

  private:
    // TODO: make this a vector<bool> by casting from Mode, but that requires ensured small linearity in Mode
    // enum values.
    std::bitset<32> ansi_;                            // AnsiMode
    std::bitset<8452 + 1> dec_;                       // DECMode
    std::map<DECMode, std::vector<bool>> savedModes_; //!< saved DEC modes
};
// }}}

// {{{ Cursor
/// Terminal cursor data structure.
///
/// NB: Take care what to store here, as DECSC/DECRC will save/restore this struct.
struct Cursor
{
    CellLocation position { LineOffset(0), ColumnOffset(0) };
    bool autoWrap = true; // false;
    bool originMode = false;
    bool visible = true;
    GraphicsAttributes graphicsRendition {};
    CharsetMapping charsets {};
    HyperlinkId hyperlink {};
    // TODO: selective erase attribute
    // TODO: SS2/SS3 states
    // TODO: CharacterSet for GL and GR
};
// }}}

/**
 * Defines the state of a terminal.
 * All those data members used to live in Screen, but are moved
 * out with the goal to move all shared state up to Terminal later
 * and have Screen API maintain only *one* screen.
 *
 * TODO: Let's move all shared data into one place,
 * ultimatively ending up in Terminal (or keep TerminalState).
 */
struct TerminalState
{
    TerminalState(Terminal& _terminal,
                  PageSize _pageSize,
                  LineCount _maxHistoryLineCount,
                  ImageSize _maxImageSize,
                  unsigned _maxImageColorRegisters,
                  bool _sixelCursorConformance,
                  ColorPalette _colorPalette,
                  bool _allowReflowOnResize);

    Terminal& terminal;

    PageSize pageSize;
    ImageSize cellPixelSize; ///< contains the pixel size of a single cell, or area(cellPixelSize_) == 0 if
                             ///< unknown.
    Margin margin;

    ColorPalette defaultColorPalette;
    ColorPalette colorPalette;

    bool focused = true;

    VTType terminalId = VTType::VT525;

    Modes modes;
    std::map<DECMode, std::vector<bool>> savedModes; //!< saved DEC modes

    unsigned maxImageColorRegisters;
    ImageSize maxImageSize;
    ImageSize maxImageSizeLimit;
    std::shared_ptr<SixelColorPalette> imageColorPalette;
    ImagePool imagePool;

    bool sixelCursorConformance = true;

    std::vector<ColumnOffset> tabs;

    bool allowReflowOnResize;

    ScreenType screenType = ScreenType::Primary;
    Grid<Cell> primaryBuffer;
    Grid<Cell> alternateBuffer;
    Grid<Cell> hostWritableStatusBuffer; // writable status-display, see DECSASD and DECSSDT.
    Grid<Cell> indicatorStatusBuffer;    // status buffer as used for indicator status line AND error lines.
    StatusDisplayType statusDisplayType;
    ActiveStatusDisplay activeStatusDisplay;

    // cursor related
    //
    Cursor cursor;
    Cursor savedCursor;
    Cursor savedPrimaryCursor;    //!< Saved cursor of primary-screen when switching to alt-screen.
    Cursor savedCursorStatusLine; //!< Saved cursor of the status line if not active, the other way around.
    CellLocation lastCursorPosition;
    bool wrapPending = false;

    CursorDisplay cursorDisplay = CursorDisplay::Steady;
    CursorShape cursorShape = CursorShape::Block;

    std::string currentWorkingDirectory = {};

    unsigned maxImageRegisterCount = 256;
    bool usePrivateColorRegisters = false;

    bool usingStdoutFastPipe = false;

    // Hyperlink related
    //
    HyperlinkStorage hyperlinks {};

    std::string windowTitle {};
    std::stack<std::string> savedWindowTitles {};

    Sequencer sequencer;
    parser::Parser<Sequencer, false> parser;
    uint64_t instructionCounter = 0;

    InputGenerator inputGenerator {};

    ViCommands viCommands;
    ViInputHandler inputHandler;

    char32_t precedingGraphicCharacter = {};
    bool terminating = false;
};

} // namespace terminal

// {{{ fmt formatters
namespace fmt
{

template <>
struct formatter<terminal::AnsiMode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::AnsiMode _mode, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}", to_string(_mode));
    }
};

template <>
struct formatter<terminal::DECMode>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::DECMode _mode, FormatContext& ctx)
    {
        return format_to(ctx.out(), "{}", to_string(_mode));
    }
};

template <>
struct formatter<terminal::Cursor>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(const terminal::Cursor cursor, FormatContext& ctx)
    {
        return format_to(ctx.out(),
                         "({}:{}{})",
                         cursor.position.line,
                         cursor.position.column,
                         cursor.visible ? "" : ", (invis)");
    }
};

template <>
struct formatter<terminal::DynamicColorName>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::DynamicColorName name, FormatContext& ctx)
    {
        // clang-format off
        using terminal::DynamicColorName;
        switch (name)
        {
        case DynamicColorName::DefaultForegroundColor: return format_to(ctx.out(), "DefaultForegroundColor");
        case DynamicColorName::DefaultBackgroundColor: return format_to(ctx.out(), "DefaultBackgroundColor");
        case DynamicColorName::TextCursorColor: return format_to(ctx.out(), "TextCursorColor");
        case DynamicColorName::MouseForegroundColor: return format_to(ctx.out(), "MouseForegroundColor");
        case DynamicColorName::MouseBackgroundColor: return format_to(ctx.out(), "MouseBackgroundColor");
        case DynamicColorName::HighlightForegroundColor: return format_to(ctx.out(), "HighlightForegroundColor");
        case DynamicColorName::HighlightBackgroundColor: return format_to(ctx.out(), "HighlightBackgroundColor");
        }
        return format_to(ctx.out(), "({})", static_cast<unsigned>(name));
        // clang-format on
    }
};

} // namespace fmt
// }}}
