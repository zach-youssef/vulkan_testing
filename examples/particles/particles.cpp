// GPU particles: a compute pass integrates 100k particles attracted to the mouse, and a graphics
// pass draws them with additive blending. The render graph orders the two, puts the compute pass
// on another queue when it can and synchronizes them; there are no barriers or semaphores here.
// The shaders' interfaces and the graph are declared in ZGAL (particles.zgal); this file supplies
// the per-frame parameters and handles input.
//
//     particles                                   interactive, in a window
//     particles --frames N [--capture out.png]    N frames headless (the smoke test)

#define VK_WRAP_UTIL_IMPL

#include "app/VulkanApp.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/ReadbackNode.h"

// Generated from particles.zgal: SimParams, Particle, the materials and ParticlesGraph
#include "particles.zgal.h"

#include <glm/glm.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "util/StbImageWrite.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <utility>

// Defined by the Makefile
#ifndef PARTICLES_SHADER_DIR
#error "PARTICLES_SHADER_DIR must be defined to the directory containing the compiled shaders"
#endif

constexpr uint32_t kParticleCount = 100'000;
// The size of the headless image
constexpr VkExtent2D kHeadlessExtent{1024, 768};

// What the user (or the smoke test) asks of the simulation. Written by input callbacks between
// frames and read when the simulation material is updated.
struct Controls {
    bool paused = false;
    bool resetRequested = true;            // The particle buffer starts out uninitialized
    std::optional<glm::vec2> mouse;        // In simulation space, while the left button is held
    std::optional<float> fixedTimestep;    // Headless runs are deterministic
    float aspect = 1.0f;                   // Width / height of what we render to
};

// The simulation's parameters for each frame (SimParams in particles.zgal), which the graph writes
// into the frame's parameter buffer. Advances the simulation's clock.
class Simulation {
public:
    explicit Simulation(Controls& controls) : controls_(&controls) {}

    SimParams next(const zgal::FrameInfo& frame) {
        const auto now = std::chrono::steady_clock::now();
        // Real time, clamped so a stall (e.g. dragging the window) doesn't fling particles away
        float dt = controls_->fixedTimestep.value_or(
            std::min(std::chrono::duration<float>(now - lastUpdate_).count(), 1.0f / 30.0f));
        lastUpdate_ = now;
        if (controls_->paused || step_ == 0) {
            dt = 0.0f;
        }
        time_ += dt;

        if (frame.surfaceExtent.width > 0 && frame.surfaceExtent.height > 0) {
            controls_->aspect = float(frame.surfaceExtent.width) / float(frame.surfaceExtent.height);
        }

        // Designated: without the mouse, the attractor wanders on a Lissajous curve
        return SimParams{
            .attractor = controls_->mouse.value_or(
                glm::vec2{0.6f * controls_->aspect * std::sin(0.4f * time_), 0.5f * std::sin(0.7f * time_)}),
            .dt = dt,
            .time = time_,
            .aspect = controls_->aspect,
            .attractorStrength = 0.15f,
            .stepIndex = step_++,
            .reset = std::exchange(controls_->resetRequested, false) ? 1u : 0u,
            .particleCount = kParticleCount,
            .particleRadius = 0.006f,
        };
    }

private:
    Controls* controls_;
    std::chrono::steady_clock::time_point lastUpdate_ = std::chrono::steady_clock::now();
    float time_ = 0.0f;
    uint32_t step_ = 0;
};

// Key presses, received from VulkanApp::runFrame's event polling, between frames
void onKey(int key, int action, Controls& controls, VulkanContext& context) {
    if (action != GLFW_PRESS) {
        return;
    }
    switch (key) {
        case GLFW_KEY_1: case GLFW_KEY_2: case GLFW_KEY_3: {
            // Takes effect at the next frame boundary; per-frame resources follow
            const uint32_t frames = static_cast<uint32_t>(key - GLFW_KEY_0);
            context.setFramesInFlight(frames);
            std::cout << "Frames in flight: " << frames << std::endl;
            break;
        }
        case GLFW_KEY_SPACE:
            controls.paused = !controls.paused;
            std::cout << (controls.paused ? "Paused" : "Running") << std::endl;
            break;
        case GLFW_KEY_R:
            controls.resetRequested = true;
            break;
        default:
            break;
    }
}

// Tracks the cursor in simulation space while the left button is held
void updateMouse(GLFWwindow* window, Controls& controls) {
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) != GLFW_PRESS) {
        controls.mouse.reset();
        return;
    }
    double x = 0.0, y = 0.0;
    int width = 1, height = 1;
    glfwGetCursorPos(window, &x, &y);
    glfwGetWindowSize(window, &width, &height);
    // Screen coordinates to [-aspect, aspect] x [-1, 1], y down like Vulkan's clip space
    controls.mouse = glm::vec2{(2.0f * float(x) / float(std::max(width, 1)) - 1.0f) * controls.aspect,
                               2.0f * float(y) / float(std::max(height, 1)) - 1.0f};
}

struct Options {
    std::optional<uint32_t> headlessFrames;
    std::optional<std::string> capturePath;
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            options.headlessFrames = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (arg == "--capture" && i + 1 < argc) {
            options.capturePath = argv[++i];
        } else {
            throw std::invalid_argument("Usage: particles [--frames N [--capture out.png]]");
        }
    }
    if (options.capturePath && !options.headlessFrames) {
        throw std::invalid_argument("--capture requires --frames");
    }
    return options;
}

// Runs the particles in a window, or headless for a number of frames; returns the last headless
// frame's pixels when capturing
Result<std::optional<std::vector<uint8_t>>> run(const Options& options, std::atomic<uint32_t>& validationMessages) {
    const bool headless = options.headlessFrames.has_value();

    AppConfig config;
    config.applicationName = "Particles";
    config.window = headless ? std::nullopt : std::optional(WindowConfig{"Particles", 1280, 800});
    config.enableValidation = true;
    config.enableSynchronizationValidation = true;
    config.logInstanceInfo = false;
    config.shaderSearchPaths = {PARTICLES_SHADER_DIR};
    config.autoReloadShaders = !headless;
    config.onValidationMessage = [&validationMessages](VkDebugUtilsMessageSeverityFlagBitsEXT, const char* message) {
        ++validationMessages;
        std::cerr << "Validation: " << message << std::endl;
    };

    VulkanApp app;
    TRY(app.init(config));
    VulkanContext& context = app.getContext();

    Controls controls;
    if (headless) {
        controls.aspect = float(kHeadlessExtent.width) / float(kHeadlessExtent.height);
        controls.fixedTimestep = 1.0f / 60.0f;
    }
    Simulation simulation(controls);

    // Prints the frame rate every couple of seconds
    auto lastReport = std::chrono::steady_clock::now();
    uint64_t lastReportFrame = 0;
    auto report = [&](FrameContext& frame) {
        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - lastReport).count();
        if (seconds >= 2.0f) {
            std::cout << float(frame.frameNumber - lastReportFrame) / seconds << " fps, "
                      << frame.context.framesInFlight() << " frames in flight" << std::endl;
            lastReport = now;
            lastReportFrame = frame.frameNumber;
        }
    };

    // The graph, ParticlesGraph in particles.zgal: the particle buffer, the per-frame parameters,
    // the simulation and the draw. Headless, it draws into an image instead of the window.
    RenderGraph& graph = app.createGraph();
    TRY_ASSIGN(const ParticlesGraph particles, ParticlesGraph::build(context, graph, ParticlesGraph::Inputs{
        .headless = headless,
        .capture = options.capturePath.has_value(),
        .width = kHeadlessExtent.width,
        .height = kHeadlessExtent.height,
        .particleCount = kParticleCount,
        .params = [&simulation](const zgal::FrameInfo& frame) { return simulation.next(frame); },
        .report = report,
    }));
    app.setActiveGraph(graph);

    if (headless) {
        // Change the number of frames in flight twice, to exercise resizing per-frame resources
        const uint32_t frames = *options.headlessFrames;
        for (uint32_t i = 0; i < frames; ++i) {
            if (i == frames / 3) {
                context.setFramesInFlight(3);
            } else if (i == 2 * frames / 3) {
                context.setFramesInFlight(1);
            }
            TRY(app.drawFrame());
        }
        if (options.capturePath && frames > 0) {
            TRY_ASSIGN(auto pixels, graph.nodeAs<ReadbackNode>(*particles.nodes.readback).pixels());
            return std::optional(std::move(pixels));
        }
    } else {
        GLFWwindow* window = app.window();
        app.setInputHandlers(InputHandlers{
            .key = [&](int key, int, int action, int) { onKey(key, action, controls, context); },
            .mouseButton = [&](int, int, int) { updateMouse(window, controls); },
            .cursorPos = [&](double, double) { updateMouse(window, controls); },
        });
        std::cout << "Controls:\n"
                     "  left mouse  attract particles to the cursor\n"
                     "  space       pause / resume\n"
                     "  R           reset the particles\n"
                     "  1, 2, 3     frames in flight\n";
        TRY(app.run());
    }
    return std::nullopt;
}

int main(int argc, char** argv) {
    Options options;
    try {
        options = parseOptions(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    std::atomic<uint32_t> validationMessages = 0;
    // Everything is destroyed when run() returns, so destruction is validated too
    auto captured = run(options, validationMessages);
    if (!captured) {
        std::cerr << "Error: " << captured.error() << std::endl;
        return EXIT_FAILURE;
    }
    if (validationMessages > 0) {
        std::cerr << validationMessages << " validation message(s)" << std::endl;
        return EXIT_FAILURE;
    }
    if (auto& pixels = *captured) {
        if (!stbi_write_png(options.capturePath->c_str(), int(kHeadlessExtent.width), int(kHeadlessExtent.height),
                            4, pixels->data(), int(kHeadlessExtent.width * 4))) {
            std::cerr << "Failed to write " << *options.capturePath << std::endl;
            return EXIT_FAILURE;
        }
        std::cout << "Wrote " << *options.capturePath << std::endl;
    }
    return EXIT_SUCCESS;
}
