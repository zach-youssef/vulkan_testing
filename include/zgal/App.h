#pragma once

#include "core/Contract.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <variant>

/*
 What the C++ that ZGAL generates for apps (dsl/, `make docs`) uses at runtime: the events an app
 handles, and the clock its frame handler runs on. Free of windowing: zgal/Window.h connects an app
 to a VulkanApp's window, and headless code calls the app's beginFrame() itself.
 */
namespace zgal {

// The keys apps can handle (ZGAL's key names: a-z, 0-9, space, escape, ...)
enum class Key {
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    Space, Escape, Enter, Tab, Backspace, Left, Right, Up, Down, Minus, Equal, KeypadAdd, KeypadSubtract,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
};

enum class MouseButton { Left, Right, Middle };

// Cursor positions are in [-1, 1] across the window, y down, like Vulkan's clip space
struct KeyPressed { Key key; };
struct KeyReleased { Key key; };
struct MousePressed { MouseButton button; glm::vec2 cursor; };
struct MouseReleased { MouseButton button; glm::vec2 cursor; };
struct CursorMoved { glm::vec2 cursor; };
struct Scrolled { glm::vec2 offset; };   // In scroll steps (lines), y up

using Event = std::variant<KeyPressed, KeyReleased, MousePressed, MouseReleased, CursorMoved, Scrolled>;

/*
 How far an app's clock advances each frame: with real time (the time since the last frame, at most
 maxStep, so a stall doesn't make the app jump; 0 on the first frame), or by a fixed step, which
 makes headless runs deterministic. A required choice: apps take one in their Inputs.
 */
class Timestep {
public:
    Timestep() = default;

    static Timestep realTime(float maxStep) {
        Timestep t;
        t.mode_ = Mode::RealTime;
        t.step_ = maxStep;
        return t;
    }

    static Timestep fixed(float step) {
        Timestep t;
        t.mode_ = Mode::Fixed;
        t.step_ = step;
        return t;
    }

    bool given() const { return mode_ != Mode::None; }

    // The time, in seconds, since the previous frame
    float next() {
        switch (mode_) {
            case Mode::Fixed:
                return step_;
            case Mode::RealTime: {
                const auto now = std::chrono::steady_clock::now();
                const float dt = last_ ? std::min(std::chrono::duration<float>(now - *last_).count(), step_) : 0.0f;
                last_ = now;
                return dt;
            }
            case Mode::None:
                break;
        }
        contractViolation("An app's Timestep wasn't given: use zgal::Timestep::realTime or fixed");
    }

private:
    enum class Mode { None, RealTime, Fixed };
    Mode mode_ = Mode::None;
    float step_ = 0.0f;
    std::optional<std::chrono::steady_clock::time_point> last_;
};

} // namespace zgal
