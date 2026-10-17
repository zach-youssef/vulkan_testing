#pragma once

#include "app/VulkanApp.h"
#include "zgal/App.h"

#include <optional>

/*
 Runs an app generated from ZGAL (dsl/, `make docs`) in a VulkanApp: its window's input becomes the
 app's events, and each frame runs the app's frame handler before drawing. The only part of ZGAL's
 runtime that knows about windows.

     auto scene = SceneApp::create(vulkanApp.getContext(), vulkanApp.createGraph(), inputs);
     return zgal::run(vulkanApp, **scene);
 */
namespace zgal {

inline std::optional<Key> keyFromGlfw(int key) {
    if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        return static_cast<Key>(static_cast<int>(Key::A) + (key - GLFW_KEY_A));
    }
    if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
        return static_cast<Key>(static_cast<int>(Key::Digit0) + (key - GLFW_KEY_0));
    }
    if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12) {
        return static_cast<Key>(static_cast<int>(Key::F1) + (key - GLFW_KEY_F1));
    }
    switch (key) {
        case GLFW_KEY_SPACE: return Key::Space;
        case GLFW_KEY_ESCAPE: return Key::Escape;
        case GLFW_KEY_ENTER: return Key::Enter;
        case GLFW_KEY_TAB: return Key::Tab;
        case GLFW_KEY_BACKSPACE: return Key::Backspace;
        case GLFW_KEY_LEFT: return Key::Left;
        case GLFW_KEY_RIGHT: return Key::Right;
        case GLFW_KEY_UP: return Key::Up;
        case GLFW_KEY_DOWN: return Key::Down;
        case GLFW_KEY_MINUS: return Key::Minus;
        case GLFW_KEY_EQUAL: return Key::Equal;
        case GLFW_KEY_KP_ADD: return Key::KeypadAdd;
        case GLFW_KEY_KP_SUBTRACT: return Key::KeypadSubtract;
        default: return std::nullopt;
    }
}

inline std::optional<MouseButton> mouseButtonFromGlfw(int button) {
    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT: return MouseButton::Left;
        case GLFW_MOUSE_BUTTON_RIGHT: return MouseButton::Right;
        case GLFW_MOUSE_BUTTON_MIDDLE: return MouseButton::Middle;
        default: return std::nullopt;
    }
}

// The cursor in [-1, 1] across the window, y down
inline glm::vec2 cursorOf(VulkanApp& vulkanApp) {
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(vulkanApp.window(), &x, &y);
    const VkExtent2D size = vulkanApp.getWindowSize();
    return {2.0f * float(x) / float(std::max(size.width, 1u)) - 1.0f,
            2.0f * float(y) / float(std::max(size.height, 1u)) - 1.0f};
}

// Sends the window's input to the app's event handlers (replacing the VulkanApp's input handlers).
// The app must outlive them.
template<typename App>
void connect(VulkanApp& vulkanApp, App& app) {
    VulkanApp* window = &vulkanApp;
    vulkanApp.setInputHandlers(InputHandlers{
        .key = [&app](int key, int, int action, int) {
            const auto k = keyFromGlfw(key);
            if (!k) {
                return;
            }
            if (action == GLFW_PRESS) {
                app.handle(Event{KeyPressed{*k}});
            } else if (action == GLFW_RELEASE) {
                app.handle(Event{KeyReleased{*k}});
            }
        },
        .mouseButton = [&app, window](int button, int action, int) {
            const auto b = mouseButtonFromGlfw(button);
            if (!b) {
                return;
            }
            const glm::vec2 cursor = cursorOf(*window);
            if (action == GLFW_PRESS) {
                app.handle(Event{MousePressed{*b, cursor}});
            } else if (action == GLFW_RELEASE) {
                app.handle(Event{MouseReleased{*b, cursor}});
            }
        },
        .cursorPos = [&app, window](double, double) { app.handle(Event{CursorMoved{cursorOf(*window)}}); },
        .scroll = [&app](double x, double y) { app.handle(Event{Scrolled{{float(x), float(y)}}}); },
    });
}

// One frame: the app's frame handler, then the VulkanApp draws the graph the app chose. For headless
// apps and loops of their own; run() is the loop for windows.
template<typename App>
Result<void> drawFrame(VulkanApp& vulkanApp, App& app) {
    TRY(app.beginFrame());
    vulkanApp.setActiveGraph(app.renderGraph());
    return vulkanApp.drawFrame();
}

// Connects the app's input, and draws frames (the graph the app chooses each frame) until the window
// closes or the app quits (quit!), or a frame fails. Requires a window.
template<typename App>
Result<void> run(VulkanApp& vulkanApp, App& app) {
    vulkanApp.setActiveGraph(app.renderGraph());
    connect(vulkanApp, app);
    while (vulkanApp.pollEvents() && !app.quitRequested()) {
        TRY(drawFrame(vulkanApp, app));
    }
    return vkCheck(vulkanApp.getContext().waitIdleAll(), "Failed to wait for the device");
}

} // namespace zgal
