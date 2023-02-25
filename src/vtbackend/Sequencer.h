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

#include <vtbackend/Functions.h>
#include <vtbackend/Image.h>
#include <vtbackend/Sequence.h>
#include <vtbackend/SixelParser.h>
#include <vtbackend/primitives.h>

#include <vtparser/ParserEvents.h>
#include <vtparser/ParserExtension.h>

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <libunicode/convert.h>
#include <libunicode/utf8.h>

namespace terminal
{

// {{{ TODO: refactor me
// XTSMGRAPHICS (xterm extension)
// CSI ? Pi ; Pa ; Pv S
namespace XtSmGraphics
{
    enum class Item
    {
        NumberOfColorRegisters = 1,
        SixelGraphicsGeometry = 2,
        ReGISGraphicsGeometry = 3,
    };

    enum class Action
    {
        Read = 1,
        ResetToDefault = 2,
        SetToValue = 3,
        ReadLimit = 4
    };

    using Value = std::variant<std::monostate, unsigned, ImageSize>;
} // namespace XtSmGraphics

/// TBC - Tab Clear
///
/// This control function clears tab stops.
enum class HorizontalTabClear
{
    /// Ps = 0 (default)
    AllTabs,

    /// Ps = 3
    UnderCursor,
};

/// Input: CSI 16 t
///
///  Input: CSI 14 t (for text area size)
///  Input: CSI 14; 2 t (for full window size)
/// Output: CSI 14 ; width ; height ; t
enum class RequestPixelSize // TODO: rename RequestPixelSize to RequestArea?
{
    CellArea,
    TextArea,
    WindowArea,
};

/// DECRQSS - Request Status String
enum class RequestStatusString
{
    SGR,
    DECSCL,
    DECSCUSR,
    DECSCA,
    DECSTBM,
    DECSLRM,
    DECSLPP,
    DECSCPP,
    DECSNLS,
    DECSASD,
    DECSSDT,
};

/// DECSIXEL - Sixel Graphics Image.
struct SixelImage
{ // TODO: this struct is only used internally in Sequencer, make it private
    /// Size in pixels for this image
    ImageSize size;

    /// RGBA buffer of the image to be rendered
    Image::Data rgba;
};

inline std::string setDynamicColorValue(
    RGBColor const& color) // TODO: yet another helper. maybe SemanticsUtils static class?
{
    auto const r = static_cast<unsigned>(static_cast<float>(color.red) / 255.0f * 0xFFFF);
    auto const g = static_cast<unsigned>(static_cast<float>(color.green) / 255.0f * 0xFFFF);
    auto const b = static_cast<unsigned>(static_cast<float>(color.blue) / 255.0f * 0xFFFF);
    return fmt::format("rgb:{:04X}/{:04X}/{:04X}", r, g, b);
}

enum class ApplyResult
{
    Ok,
    Invalid,
    Unsupported,
};
// }}}

class Terminal;

/// Sequencer - The semantic VT analyzer layer.
///
/// Sequencer implements the translation from VT parser events, forming a higher level Sequence,
/// that can be matched against actions to perform on the target Screen.
class Sequencer
{
  public:
    /// Constructs the sequencer stage.
    explicit Sequencer(Terminal& terminal);

    // ParserEvents
    //
    void error(std::string_view errorString);
    void print(char32_t codepoint);
    size_t print(std::string_view chars, size_t cellCount);
    void execute(char controlCode);
    void clear() noexcept;
    void collect(char ch);
    void collectLeader(char leader) noexcept;
    void param(char ch) noexcept;
    void paramDigit(char ch) noexcept;
    void paramSeparator() noexcept;
    void paramSubSeparator() noexcept;
    void dispatchESC(char finalChar);
    void dispatchCSI(char finalChar);
    void startOSC();
    void putOSC(char ch);
    void dispatchOSC();
    void hook(char finalChar);
    void put(char ch);
    void unhook();
    void startAPC() {}
    void putAPC(char) {}
    void dispatchAPC() {}
    void startPM() {}
    void putPM(char) {}
    void dispatchPM() {}

    void hookParser(std::unique_ptr<ParserExtension> parserExtension) noexcept
    {
        _hookedParser = std::move(parserExtension);
    }

    [[nodiscard]] size_t maxBulkTextSequenceWidth() const noexcept;

  private:
    void handleSequence();

    // private data
    //
    Terminal& _terminal;
    Sequence _sequence {};
    SequenceParameterBuilder _parameterBuilder;

    std::unique_ptr<ParserExtension> _hookedParser;
    std::unique_ptr<SixelImageBuilder> _sixelImageBuilder;
};

// {{{ inlines
inline void Sequencer::clear() noexcept
{
    _sequence.clearExceptParameters();
    _parameterBuilder.reset();
}

inline void Sequencer::paramDigit(char ch) noexcept
{
    _parameterBuilder.multiplyBy10AndAdd(static_cast<uint8_t>(ch - '0'));
}

inline void Sequencer::paramSeparator() noexcept
{
    _parameterBuilder.nextParameter();
}

inline void Sequencer::paramSubSeparator() noexcept
{
    _parameterBuilder.nextSubParameter();
}
// }}}

} // namespace terminal

// {{{ fmt formatter
namespace fmt
{

template <>
struct formatter<terminal::RequestStatusString>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    constexpr auto format(terminal::RequestStatusString value, FormatContext& ctx)
    {
        switch (value)
        {
            case terminal::RequestStatusString::SGR: return fmt::format_to(ctx.out(), "SGR");
            case terminal::RequestStatusString::DECSCL: return fmt::format_to(ctx.out(), "DECSCL");
            case terminal::RequestStatusString::DECSCUSR: return fmt::format_to(ctx.out(), "DECSCUSR");
            case terminal::RequestStatusString::DECSCA: return fmt::format_to(ctx.out(), "DECSCA");
            case terminal::RequestStatusString::DECSTBM: return fmt::format_to(ctx.out(), "DECSTBM");
            case terminal::RequestStatusString::DECSLRM: return fmt::format_to(ctx.out(), "DECSLRM");
            case terminal::RequestStatusString::DECSLPP: return fmt::format_to(ctx.out(), "DECSLPP");
            case terminal::RequestStatusString::DECSCPP: return fmt::format_to(ctx.out(), "DECSCPP");
            case terminal::RequestStatusString::DECSNLS: return fmt::format_to(ctx.out(), "DECSNLS");
            case terminal::RequestStatusString::DECSASD: return fmt::format_to(ctx.out(), "DECSASD");
            case terminal::RequestStatusString::DECSSDT: return fmt::format_to(ctx.out(), "DECSSDT");
        }
        return fmt::format_to(ctx.out(), "{}", unsigned(value));
    }
};

template <>
struct formatter<terminal::Sequence>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(terminal::Sequence const& seq, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", seq.text());
    }
};
} // namespace fmt
