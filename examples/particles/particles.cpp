// GPU particles: a compute pass integrates 100k particles attracted to the mouse, and a graphics
// pass draws them with additive blending. The render graph orders the two, puts the compute pass
// on another queue when it can and synchronizes them; there are no barriers or semaphores here.
// The shaders, the graph and the app (its controls, clock and input handlers) are declared in ZGAL
// (particles.zgal); this file sets up the window and runs the app.
//
//     particles                                   interactive, in a window
//     particles --frames N [--capture out.png]    N frames headless (the smoke test)

#define VK_WRAP_UTIL_IMPL

#include "app/VulkanApp.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/ReadbackNode.h"
#include "zgal/Window.h"

// Generated from particles.zgal, in namespace particles: SimParams, Particle, the materials,
// ParticlesGraph and ParticlesApp
#include "particles.zgal.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "util/StbImageWrite.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <utility>

constexpr uint32_t kParticleCount = 100'000;
// The size of the headless image
constexpr VkExtent2D kHeadlessExtent{1024, 768};

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
    config.autoReloadShaders = !headless;
    config.onValidationMessage = [&validationMessages](VkDebugUtilsMessageSeverityFlagBitsEXT, const char* message) {
        ++validationMessages;
        std::cerr << "Validation: " << message << std::endl;
    };

    VulkanApp app;
    TRY(app.init(config));
    VulkanContext& context = app.getContext();

    // The app (ParticlesApp in particles.zgal) builds its graph: the particle buffer, the per-frame
    // parameters computed from its state, the simulation and the draw. Headless, it draws into an
    // image instead of the window, and its clock advances a fixed step each frame.
    RenderGraph& graph = app.createGraph();
    TRY_ASSIGN(auto particlesApp, particles::ParticlesApp::create(context, graph, particles::ParticlesApp::Inputs{
        .headless = headless,
        .capture = options.capturePath.has_value(),
        .width = kHeadlessExtent.width,
        .height = kHeadlessExtent.height,
        .particleCount = kParticleCount,
        .timestep = headless ? zgal::Timestep::fixed(1.0f / 60.0f) : zgal::Timestep::realTime(1.0f / 30.0f),
    }));

    if (headless) {
        app.setActiveGraph(graph);
        // Change the number of frames in flight twice, to exercise resizing per-frame resources
        const uint32_t frames = *options.headlessFrames;
        for (uint32_t i = 0; i < frames; ++i) {
            if (i == frames / 3) {
                context.setFramesInFlight(3);
            } else if (i == 2 * frames / 3) {
                context.setFramesInFlight(1);
            }
            TRY(zgal::drawFrame(app, *particlesApp));
        }
        if (options.capturePath && frames > 0) {
            TRY_ASSIGN(auto pixels, graph.nodeAs<ReadbackNode>(*particlesApp->ids().nodes.readback).pixels());
            return std::optional(std::move(pixels));
        }
    } else {
        std::cout << "Controls:\n"
                     "  left mouse  attract particles to the cursor\n"
                     "  space       pause / resume\n"
                     "  R           reset the particles\n"
                     "  1, 2, 3     frames in flight\n";
        TRY(zgal::run(app, *particlesApp));
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
