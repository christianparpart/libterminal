/**
 * This file is part of the "contour" project
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

#include <contour/Config.h>

#include <vtbackend/InputGenerator.h>
#include <vtbackend/ScreenEvents.h>

#include <vtrasterizer/GridMetrics.h>

#include <crispy/logstore.h>

#include <QtCore/QCoreApplication>
#include <QtCore/Qt>
#include <QtGui/QKeyEvent>
#include <QtGui/QScreen>
#include <QtQml/QQmlApplicationEngine>
#include <QtQuick/QQuickWindow>

#include <cctype>
#include <map>
#include <string>
#include <string_view>

namespace terminal::rasterizer
{
class Renderer;
}

namespace contour
{

auto inline const DisplayLog =
    logstore::Category("gui.display", "Logs display driver details (e.g. OpenGL).");
auto inline const InputLog =
    logstore::Category("gui.input", "Logs input driver details (e.g. GUI input events).");
auto inline const SessionLog = logstore::Category("gui.session", "VT terminal session logs");

namespace detail
{
    template <typename F>
    class FunctionCallEvent: public QEvent
    {
      private:
        using Fun = typename std::decay<F>::type;
        Fun fun;

      public:
        FunctionCallEvent(Fun&& fun): QEvent(QEvent::None), fun(std::move(fun)) {}
        FunctionCallEvent(Fun const& fun): QEvent(QEvent::None), fun(fun) {}
        ~FunctionCallEvent() override { fun(); }
    };
} // namespace detail

enum class MouseCursorShape
{
    Hidden,
    PointingHand,
    IBeam,
    Arrow,
};

template <typename F>
void postToObject(QObject* obj, F fun)
{
#if 0
    // Qt >= 5.10
    QMetaObject::invokeMethod(obj, std::forward<F>(fun));
#else
    // Qt < 5.10
    QCoreApplication::postEvent(obj, new detail::FunctionCallEvent<F>(std::forward<F>(fun)));
#endif
}

constexpr inline bool isModifier(Qt::Key _key)
{
    switch (_key)
    {
        case Qt::Key_Alt:
        case Qt::Key_Control:
        case Qt::Key_Shift:
        case Qt::Key_Meta: return true;
        default: return false;
    }
}

constexpr inline char32_t makeChar(Qt::Key _key, Qt::KeyboardModifiers _mods)
{
    auto const value = static_cast<int>(_key);
    if (value >= 'A' && value <= 'Z')
    {
        if (_mods & Qt::ShiftModifier)
            return static_cast<char32_t>(value);
        else
            return static_cast<char32_t>(std::tolower(value));
    }
    return 0;
}

constexpr inline terminal::modifier makeModifier(Qt::KeyboardModifiers _mods)
{
    using terminal::modifier;

    modifier mods {};

    if (_mods & Qt::AltModifier)
        mods |= modifier::Alt;
    if (_mods & Qt::ShiftModifier)
        mods |= modifier::Shift;
#if defined(__APPLE__)
    // XXX https://doc.qt.io/qt-5/qt.html#KeyboardModifier-enum
    //     "Note: On macOS, the ControlModifier value corresponds to the Command keys on the keyboard,
    //      and the MetaModifier value corresponds to the Control keys."
    if (_mods & Qt::MetaModifier)
        mods |= modifier::Control;
    if (_mods & Qt::ControlModifier)
        mods |= modifier::Meta;
#else
    if (_mods & Qt::ControlModifier)
        mods |= modifier::Control;
    if (_mods & Qt::MetaModifier)
        mods |= modifier::Meta;
#endif

    return mods;
}

constexpr inline terminal::mouse_button makeMouseButton(Qt::MouseButton _button)
{
    switch (_button)
    {
        case Qt::MouseButton::RightButton: return terminal::mouse_button::Right;
        case Qt::MiddleButton: return terminal::mouse_button::Middle;
        case Qt::LeftButton: [[fallthrough]];
        default: // d'oh
            return terminal::mouse_button::Left;
    }
}

class TerminalSession;
bool sendKeyEvent(QKeyEvent* _keyEvent, TerminalSession& _session);
void sendWheelEvent(QWheelEvent* _event, TerminalSession& _session);
void sendMousePressEvent(QMouseEvent* _event, TerminalSession& _session);
void sendMouseMoveEvent(QMouseEvent* _event, TerminalSession& _session);
void sendMouseMoveEvent(QHoverEvent* _event, TerminalSession& _session);
void sendMouseReleaseEvent(QMouseEvent* _event, TerminalSession& _session);

void spawnNewTerminal(std::string const& _programPath,
                      std::string const& _configPath,
                      std::string const& _profileName,
                      std::string const& _cwdUrl);

terminal::FontDef getFontDefinition(terminal::rasterizer::Renderer& _renderer);

terminal::rasterizer::PageMargin computeMargin(terminal::image_size _cellSize,
                                               terminal::PageSize _charCells,
                                               terminal::image_size _pixels) noexcept;

terminal::rasterizer::FontDescriptions sanitizeFontDescription(terminal::rasterizer::FontDescriptions _fonts,
                                                               text::DPI _screenDPI);

constexpr terminal::PageSize pageSizeForPixels(crispy::ImageSize viewSize,
                                               crispy::ImageSize cellSize) noexcept
{
    return terminal::PageSize { boxed_cast<terminal::LineCount>((viewSize / cellSize).height),
                                boxed_cast<terminal::ColumnCount>((viewSize / cellSize).width) };
}

void applyResize(terminal::image_size _newPixelSize,
                 TerminalSession& _session,
                 terminal::rasterizer::Renderer& _renderer);

bool applyFontDescription(terminal::image_size _cellSize,
                          terminal::PageSize _pageSize,
                          terminal::image_size _pixelSize,
                          text::DPI _dpi,
                          terminal::rasterizer::Renderer& _renderer,
                          terminal::rasterizer::FontDescriptions _fontDescriptions);

constexpr Qt::CursorShape toQtMouseShape(MouseCursorShape _shape)
{
    switch (_shape)
    {
        case contour::MouseCursorShape::Hidden: return Qt::CursorShape::BlankCursor;
        case contour::MouseCursorShape::Arrow: return Qt::CursorShape::ArrowCursor;
        case contour::MouseCursorShape::IBeam: return Qt::CursorShape::IBeamCursor;
        case contour::MouseCursorShape::PointingHand: return Qt::CursorShape::PointingHandCursor;
    }

    // should never be reached
    return Qt::CursorShape::ArrowCursor;
}

/// Declares the screen-dirtiness-vs-rendering state.
enum class RenderState
{
    CleanIdle,     //!< No screen updates and no rendering currently in progress.
    DirtyIdle,     //!< Screen updates pending and no rendering currently in progress.
    CleanPainting, //!< No screen updates and rendering currently in progress.
    DirtyPainting  //!< Screen updates pending and rendering currently in progress.
};

/// Defines the current screen-dirtiness-vs-rendering state.
///
/// This is primarily updated by two independant threads, the rendering thread and the I/O
/// thread.
/// The rendering thread constantly marks the rendering state CleanPainting whenever it is about
/// to render and, depending on whether new screen changes happened, in the frameSwapped()
/// callback either DirtyPainting and continues to rerender or CleanIdle if no changes came in
/// since last render.
///
/// The I/O thread constantly marks the state dirty whenever new data has arrived,
/// either DirtyIdle if no painting is currently in progress, DirtyPainting otherwise.
struct RenderStateManager
{
    std::atomic<RenderState> state_ = RenderState::CleanIdle;
    bool renderingPressure_ = false;

    RenderState fetchAndClear() { return state_.exchange(RenderState::CleanPainting); }

    bool touch()
    {
        for (;;)
        {
            auto state = state_.load();
            switch (state)
            {
                case RenderState::CleanIdle:
                    if (state_.compare_exchange_strong(state, RenderState::DirtyIdle))
                        return true;
                    break;
                case RenderState::CleanPainting:
                    if (state_.compare_exchange_strong(state, RenderState::DirtyPainting))
                        return false;
                    break;
                case RenderState::DirtyIdle:
                case RenderState::DirtyPainting: return false;
            }
        }
    }

    /// @retval true finished rendering, nothing pending yet. So please start update timer.
    /// @retval false more data pending. Rerun paint() immediately.
    bool finish()
    {
        for (;;)
        {
            auto state = state_.load();
            switch (state)
            {
                case RenderState::DirtyIdle:
                    // assert(!"The impossible happened, painting but painting. Shakesbeer.");
                    // qDebug() << "The impossible happened, onFrameSwapped() called in wrong state
                    // DirtyIdle.";
                    [[fallthrough]];
                case RenderState::DirtyPainting: return false;
                case RenderState::CleanPainting:
                    if (!state_.compare_exchange_strong(state, RenderState::CleanIdle))
                        break;
                    [[fallthrough]];
                case RenderState::CleanIdle: renderingPressure_ = false; return true;
            }
        }
    }
};

#define CONSUME_GL_ERRORS()                         \
    do                                              \
    {                                               \
        GLenum err {};                              \
        while ((err = glGetError()) != GL_NO_ERROR) \
            ;                                       \
    } while (0)

#define CHECKED_GL(code)                                            \
    do                                                              \
    {                                                               \
        (code);                                                     \
        GLenum err {};                                              \
        while ((err = glGetError()) != GL_NO_ERROR)                 \
            errorlog()("OpenGL error {} for call: {}", err, #code); \
    } while (0)

} // namespace contour

template <>
struct fmt::formatter<contour::RenderState>: public fmt::formatter<std::string>
{
    using State = contour::RenderState;
    static auto format(State state, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (state)
        {
            case State::CleanIdle: name = "clean-idle"; break;
            case State::CleanPainting: name = "clean-painting"; break;
            case State::DirtyIdle: name = "dirty-idle"; break;
            case State::DirtyPainting: name = "dirty-painting"; break;
        }
        return fmt::formatter<string_view> {}.format(name, ctx);
    }
};
