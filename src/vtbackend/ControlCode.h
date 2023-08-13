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

#include <limits>
#include <string_view>

namespace terminal::ControlCode
{

enum class c0 : char
{
    NUL = 0x00, //!< Null
    SOH = 0x01, //!< Start of Heading
    STX = 0x02, //!< Start of Text
    ETX = 0x03, //!< End of Text
    EOT = 0x04, //!< End of Transmission
    ENQ = 0x05, //!< Enquiry
    ACK = 0x06, //!< Acknowledge
    BEL = 0x07, //!< Bell, Alert
    BS = 0x08,  //!< Backspace
    HT = 0x09,  //!< Horizontal Tab
    LF = 0x0A,  //!< Line Feed
    VT = 0x0B,  //!< Vertical Tab
    FF = 0x0C,  //!< Form Feed
    CR = 0x0D,  //!< Carriage Return
    SO = 0x0E,  //!< Shift Out
    SI = 0x0F,  //!< Shift In
    DLE = 0x10, //!< Data Link Escape
    DC1 = 0x11, //!< Device Control One
    DC2 = 0x12, //!< Device Control Two
    DC3 = 0x13, //!< Device Control Three
    DC4 = 0x14, //!< Device Control Four
    NAK = 0x15, //!< Negative Acknowledge
    SYN = 0x16, //!< Synchronous Idle
    ETB = 0x17, //!< End of Transmission Block
    CAN = 0x18, //!< Cancel
    EM = 0x19,  //!< End of Medium
    SUB = 0x1A, //!< Substitute
    ESC = 0x1B, //!< Escape
    FS = 0x1C,  //!< File Separator
    GS = 0x1D,  //!< Group Separator
    RS = 0x1E,  //!< Record Separator
    US = 0x1F,  //!< Unit Separator

    // Not part of C0, but meant to be always available.
    SP = 0x20,  //!< Space
    DEL = 0x7F, //!< Delete
};

// NOLINTBEGIN(readability-identifier-naming)
enum class C1_7bit
{
    SCS_G0 = 0x28,       //!< Set Character Set (0)
    SCS_G1 = 0x29,       //!< Set Character Set (1)
    SCS_G2 = 0x2a,       //!< Set Character Set (2)
    SCS_G3 = 0x2b,       //!< Set Character Set (3)
    DECSC = 0x37,        //!< Save Cursor
    DECRC = 0x38,        //!< Restore Cursor
    NOT_DEFINED = 0x40,  //!< Not Defined
    NOT_DEFINED1 = 0x41, //!< Not Defined (1)
    BPH = 0x42,          //!< Break Permitted Here
    NBH = 0x43,          //!< No Break Here
    IND = 0x44,          //!< Index
    NEL = 0x45,          //!< Next Line
    SSA = 0x46,          //!< Start of Selected Area
    ESA = 0x47,          //!< End of Selected Area
    HTS = 0x48,          //!< Horizontal Tabulation Set
    HTJ = 0x49,          //!< Horizontal Tabulation with Justification
    VTS = 0x4a,          //!< Vertical Tabulation Set
    PLD = 0x4b,          //!< Partial Line Down
    PLU = 0x4c,          //!< Partial Line Up
    RI = 0x4d,           //!< Reverse Index
    SS2 = 0x4e,          //!< Single Shift 2
    SS3 = 0x4f,          //!< Single Shift 3
    DCS = 0x50,          //!< Device Control String
    PU1 = 0x51,          //!< Private Use 1
    PU2 = 0x52,          //!< Private Use 2
    STS = 0x53,          //!< Set Transmit State
    CCH = 0x54,          //!< Cancel character
    MW = 0x55,           //!< Message Waiting
    SPA = 0x56,          //!< Start of Protected Area
    EPA = 0x57,          //!< End of Protected Area
    SOS = 0x58,          //!< Start of String
    NOT_DEFINED3 = 0x59, //!< Not Defined (3)
    SCI = 0x5a,          //!< Single Character Introducer
    CSI = 0x5b,          //!< Control Sequence Introducer
    ST = 0x5c,           //!< String Terminator
    OSC = 0x5d,          //!< Operating System Command
    PM = 0x5e,           //!< Privacy Message
    APC = 0x5f,          //!< Application Program Command
};

enum class C1_8bit
{
    PAD = 0x80,  //!< Padding Character
    HOP = 0x81,  //!< High Octet Preset
    BPH = 0x82,  //!< Break Permitted Here
    NBH = 0x83,  //!< No Break Here
    IND = 0x84,  //!< Index
    NEL = 0x85,  //!< Next Line
    SSA = 0x86,  //!< Start of Selected Area
    ESA = 0x87,  //!< End of Selected Area
    HTS = 0x88,  //!< Horizontal Tabulation Set
    HTJ = 0x89,  //!< Horizontal Tabulation With Justification
    VTS = 0x8A,  //!< Vertical Tabulation Set
    PLD = 0x8B,  //!< Partial Line Down
    PLU = 0x8C,  //!< Partial Line Up
    RI = 0x8D,   //!< Reverse Index
    SS2 = 0x8E,  //!< Single Shift 2
    SS3 = 0x8F,  //!< Single Shift 3
    DCS = 0x90,  //!< Device Control String
    PU1 = 0x91,  //!< Private Use 1
    PU2 = 0x92,  //!< Private Use 2
    STS = 0x93,  //!< Set Transmit State
    CCH = 0x94,  //!< Cancel character
    MW = 0x95,   //!< Message Waiting
    SPA = 0x96,  //!< Start of Protected Area
    EPA = 0x97,  //!< End of Protected Area
    SOS = 0x98,  //!< Start of String
    SGCI = 0x99, //!< Single Graphic Character Introducer
    SCI = 0x9A,  //!< Single Character Introducer
    CSI = 0x9B,  //!< Control Sequence Introducer
    ST = 0x9C,   //!< String Terminator
    OSC = 0x9D,  //!< Operating System Command
    PM = 0x9E,   //!< Privacy Message
    APC = 0x9F,  //!< Application Program Command
};
// NOLINTEND(readability-identifier-naming)

constexpr std::string_view to_short_string(c0 code)
{
    switch (code)
    {
        case c0::NUL: return "NUL";
        case c0::SOH: return "SOH";
        case c0::STX: return "STX";
        case c0::ETX: return "ETX";
        case c0::EOT: return "EOT";
        case c0::ENQ: return "ENQ";
        case c0::ACK: return "ACK";
        case c0::BEL: return "BEL";
        case c0::BS: return "BS";
        case c0::HT: return "HT";
        case c0::LF: return "LF";
        case c0::VT: return "VT";
        case c0::FF: return "FF";
        case c0::CR: return "CR";
        case c0::SO: return "SO";
        case c0::SI: return "SI";
        case c0::DLE: return "DLE";
        case c0::DC1: return "DC1";
        case c0::DC2: return "DC2";
        case c0::DC3: return "DC3";
        case c0::DC4: return "DC4";
        case c0::NAK: return "NAK";
        case c0::SYN: return "SYN";
        case c0::ETB: return "ETB";
        case c0::CAN: return "CAN";
        case c0::EM: return "EM";
        case c0::SUB: return "SUB";
        case c0::ESC: return "ESC";
        case c0::FS: return "FS";
        case c0::GS: return "GS";
        case c0::RS: return "RS";
        case c0::US: return "US";
        case c0::SP: return "SP";
        case c0::DEL: return "DEL";
        default: return "?";
    }
}

constexpr std::string_view to_string(c0 code)
{
    switch (code)
    {
        case c0::NUL: return "Null";
        case c0::SOH: return "Start of Heading";
        case c0::STX: return "Start of Text";
        case c0::ETX: return "End of Text";
        case c0::EOT: return "End of Transmission";
        case c0::ENQ: return "Enquiry";
        case c0::ACK: return "Acknowledge";
        case c0::BEL: return "Bell, Alert";
        case c0::BS: return "Backspace";
        case c0::HT: return "Horizontal Tab";
        case c0::LF: return "Line Feed";
        case c0::VT: return "Vertical Tab";
        case c0::FF: return "Form Feed";
        case c0::CR: return "Carriage Return";
        case c0::SO: return "Shift Out";
        case c0::SI: return "Shift In";
        case c0::DLE: return "Data Link Escape";
        case c0::DC1: return "Device Control One";
        case c0::DC2: return "Device Control Two";
        case c0::DC3: return "Device Control Three";
        case c0::DC4: return "Device Control Four";
        case c0::NAK: return "Negative Acknowledge";
        case c0::SYN: return "Synchronous Idle";
        case c0::ETB: return "End of Transmission Block";
        case c0::CAN: return "Cancel";
        case c0::EM: return "End of Medium";
        case c0::SUB: return "Substitute";
        case c0::ESC: return "Escape";
        case c0::FS: return "File Separator";
        case c0::GS: return "Group Separator";
        case c0::RS: return "Record Separator";
        case c0::US: return "Unit Separator";
        case c0::SP: return "Space";
        case c0::DEL: return "Delete";
        default: return "?";
    }
}

constexpr std::string_view to_string(C1_7bit code)
{
    switch (code)
    {
        case C1_7bit::SCS_G0: return "Set Character Set (0)";
        case C1_7bit::SCS_G1: return "Set Character Set (1)";
        case C1_7bit::SCS_G2: return "Set Character Set (2)";
        case C1_7bit::SCS_G3: return "Set Character Set (3)";
        case C1_7bit::DECSC: return "Save Cursor";
        case C1_7bit::DECRC: return "Restore Cursor";
        case C1_7bit::NOT_DEFINED: return "Not Defined";
        case C1_7bit::NOT_DEFINED1: return "Not Defined (1)";
        case C1_7bit::BPH: return "Break Permitted Here";
        case C1_7bit::NBH: return "No Break Here";
        case C1_7bit::IND: return "Index";
        case C1_7bit::NEL: return "Next Line";
        case C1_7bit::SSA: return "Start of Selected Area";
        case C1_7bit::ESA: return "End of Selected Area";
        case C1_7bit::HTS: return "Horizontal Tabulation Set";
        case C1_7bit::HTJ: return "Horizontal Tabulation with Justification";
        case C1_7bit::VTS: return "Vertical Tabulation Set";
        case C1_7bit::PLD: return "Partial Line Down";
        case C1_7bit::PLU: return "Partial Line Up";
        case C1_7bit::RI: return "Reverse Index";
        case C1_7bit::SS2: return "Single Shift 2";
        case C1_7bit::SS3: return "Single Shift 3";
        case C1_7bit::DCS: return "Device Control String";
        case C1_7bit::PU1: return "Private Use 1";
        case C1_7bit::PU2: return "Private Use 2";
        case C1_7bit::STS: return "Set Transmit State";
        case C1_7bit::CCH: return "Cancel character";
        case C1_7bit::MW: return "Message Waiting";
        case C1_7bit::SPA: return "Start of Protected Area";
        case C1_7bit::EPA: return "End of Protected Area";
        case C1_7bit::SOS: return "Start of String";
        case C1_7bit::NOT_DEFINED3: return "Not Defined (3)";
        case C1_7bit::SCI: return "Single Character Introducer";
        case C1_7bit::CSI: return "Control Sequence Introducer";
        case C1_7bit::ST: return "String Terminator";
        case C1_7bit::OSC: return "Operating System Command";
        case C1_7bit::PM: return "Privacy Message";
        case C1_7bit::APC: return "Application Program Command";
    }
}

constexpr std::string_view to_string(C1_8bit code)
{
    switch (code)
    {
        case C1_8bit::PAD: return "Padding Character";
        case C1_8bit::HOP: return "High Octet Preset";
        case C1_8bit::BPH: return "Break Permitted Here";
        case C1_8bit::NBH: return "No Break Here";
        case C1_8bit::IND: return "Index";
        case C1_8bit::NEL: return "Next Line";
        case C1_8bit::SSA: return "Start of Selected Area";
        case C1_8bit::ESA: return "End of Selected Area";
        case C1_8bit::HTS: return "Horizontal Tabulation Set";
        case C1_8bit::HTJ: return "Horizontal Tabulation With Justification";
        case C1_8bit::VTS: return "Vertical Tabulation Set";
        case C1_8bit::PLD: return "Partial Line Down";
        case C1_8bit::PLU: return "Partial Line Up";
        case C1_8bit::RI: return "Reverse Index";
        case C1_8bit::SS2: return "Single Shift 2";
        case C1_8bit::SS3: return "Single Shift 3";
        case C1_8bit::DCS: return "Device Control String";
        case C1_8bit::PU1: return "Private Use 1";
        case C1_8bit::PU2: return "Private Use 2";
        case C1_8bit::STS: return "Set Transmit State";
        case C1_8bit::CCH: return "Cancel character";
        case C1_8bit::MW: return "Message Waiting";
        case C1_8bit::SPA: return "Start of Protected Area";
        case C1_8bit::EPA: return "End of Protected Area";
        case C1_8bit::SOS: return "Start of String";
        case C1_8bit::SGCI: return "Single Graphic Character Introducer";
        case C1_8bit::SCI: return "Single Character Introducer";
        case C1_8bit::CSI: return "Control Sequence Introducer";
        case C1_8bit::ST: return "String Terminator";
        case C1_8bit::OSC: return "Operating System Command";
        case C1_8bit::PM: return "Privacy Message";
        case C1_8bit::APC: return "Application Program Command";
        default: return "?";
    }
}

} // namespace terminal::ControlCode

namespace std
{

template <>
struct numeric_limits<terminal::ControlCode::c0>
{
    static constexpr size_t min() noexcept { return 0x00; }
    static constexpr size_t max() noexcept { return 0x1F; }
};

template <>
struct numeric_limits<terminal::ControlCode::C1_7bit>
{
    static constexpr size_t min() noexcept { return 0x28; }
    static constexpr size_t max() noexcept { return 0x5F; }
};

template <>
struct numeric_limits<terminal::ControlCode::C1_8bit>
{
    static constexpr size_t min() noexcept { return 0x80; }
    static constexpr size_t max() noexcept { return 0x9F; }
};

} // namespace std
