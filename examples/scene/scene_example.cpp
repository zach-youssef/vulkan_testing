// A lit 3D scene: the example of a "normal" forward renderer. Each frame runs three passes,
//   1. shadow map: the scene's depth from a directional light, into a fixed-size depth image
//   2. lit:        PBR shading with filtered shadows into a linear HDR image the size of the window
//   3. tonemap:    exposure and an ACES curve from the HDR image to the window (or, headless, to an
//                  image read back into a PNG)
// and the render graph derives their order, barriers and queues from what each pass uses. The
// shaders, the graph and the app (the camera, the sun and the input handlers) are declared in ZGAL
// (scene.zgal); this file supplies the meshes, sets up the window and runs the app.
//
//   scene_example                                     interactive, in a window
//   scene_example --frames N [--capture out.png] [--size WxH]
//                                                     headless smoke test: N frames with validation
//                                                     and synchronization validation, exiting
//                                                     nonzero on any validation message, resizing
//                                                     the view halfway
//   scene_example --window --frames N                 the same in a window, resizing it halfway

// Vulkan's clip space depth is [0, 1] (GLM defaults to OpenGL's [-1, 1]). Defined before anything
// includes GLM.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE

#define VK_WRAP_UTIL_IMPL

#include "app/VulkanApp.h"
#include "descriptors/DescriptorHeap.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/ReadbackNode.h"
#include "graph/nodes/RenderablesNode.h"
#include "materials/GraphicsMaterial.h"
#include "materials/Renderable.h"
#include "shaders/ShaderLibrary.h"

#include "Meshes.h"
// Generated from scene.zgal, in namespace scene: SceneUniforms, SceneVertex, the Shadow, Lit and
// Tonemap materials, SceneGraph and SceneApp
#include "scene.zgal.h"
#include "zgal/Window.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
// For --capture
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "util/StbImageWrite.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>

// Defined by the Makefile
#ifndef SCENE_EXAMPLE_SHADER_DIR
#error "SCENE_EXAMPLE_SHADER_DIR must be defined to the directory containing the compiled shaders"
#endif
const std::string shaderDir = SCENE_EXAMPLE_SHADER_DIR;

// ---------------------------------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------------------------------

std::vector<MeshData> buildScene() {
    using glm::vec3;
    auto box = [](vec3 centre, vec3 size, float yawDegrees, SurfaceMaterial material) {
        glm::mat4 transform = glm::translate(glm::mat4(1.0f), centre);
        transform = glm::rotate(transform, glm::radians(yawDegrees), vec3(0.0f, 1.0f, 0.0f));
        return meshes::cube(material, glm::scale(transform, size));
    };
    // Albedos are linear (sRGB colours would be decoded first)
    return {
        meshes::plane(40.0f, {vec3(0.4f, 0.4f, 0.38f), 0.9f}),
        box({-2.2f, 0.5f, 0.2f}, vec3(1.0f), 20.0f, {vec3(0.75f, 0.08f, 0.06f), 0.5f}),
        box({2.2f, 0.75f, -1.2f}, vec3(1.5f), -35.0f, {vec3(0.08f, 0.2f, 0.7f), 0.35f}),
        box({-0.4f, 1.25f, -3.0f}, vec3(0.7f, 2.5f, 0.7f), 10.0f, {vec3(0.8f, 0.6f, 0.1f), 0.7f}),
        meshes::sphere({0.2f, 0.8f, 1.2f}, 0.8f, {vec3(0.9f, 0.9f, 0.9f), 0.15f}),
        meshes::sphere({-2.6f, 0.55f, 2.4f}, 0.55f, {vec3(0.1f, 0.6f, 0.15f), 0.85f}),
        meshes::sphere({2.8f, 0.5f, 1.8f}, 0.5f, {vec3(0.9f, 0.35f, 0.05f), 0.3f}),
    };
}

struct Options {
    std::optional<uint64_t> frames;      // Run this many frames, then exit
    std::optional<std::string> capture;  // Headless: write the last frame to this PNG
    VkExtent2D size{1280, 720};
    bool window = true;
};

Options parseOptions(int argc, char** argv) {
    Options options;
    bool forceWindow = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(arg + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--frames") {
            options.frames = std::stoull(value());
        } else if (arg == "--capture") {
            options.capture = value();
        } else if (arg == "--size") {
            const std::string size = value();
            const size_t x = size.find('x');
            if (x == std::string::npos) {
                throw std::invalid_argument("--size takes WxH, e.g. 640x360");
            }
            options.size = {static_cast<uint32_t>(std::stoul(size.substr(0, x))),
                            static_cast<uint32_t>(std::stoul(size.substr(x + 1)))};
        } else if (arg == "--window") {
            forceWindow = true;
        } else {
            throw std::invalid_argument("Unknown argument " + arg
                                        + "\nUsage: scene_example [--frames N [--capture out.png] [--size WxH] [--window]]");
        }
    }
    // A fixed number of frames is a smoke test, headless unless asked otherwise
    options.window = !options.frames || forceWindow;
    if (options.capture && options.window) {
        throw std::invalid_argument("--capture is headless only");
    }
    return options;
}

// ---------------------------------------------------------------------------------------------------

// Runs the example, counting validation messages into validationMessages (which outlives the app:
// destroying the context can report, e.g. leaks). Returns the last frame's pixels when capturing.
Result<std::optional<std::vector<uint8_t>>> run(const Options& options, std::atomic<uint32_t>& validationMessages) {
    const bool headless = !options.window;

    AppConfig config;
    config.applicationName = "Scene example";
    config.window = headless ? std::nullopt : std::optional<WindowConfig>(WindowConfig{"Scene example", options.size.width,
                                                                                       options.size.height});
    config.enableValidation = true;
    // Synchronization validation is slow; smoke tests (a fixed number of frames) turn it on
    config.enableSynchronizationValidation = options.frames.has_value();
    config.onValidationMessage = [&validationMessages](VkDebugUtilsMessageSeverityFlagBitsEXT, const char* message) {
        ++validationMessages;
        std::cerr << "Validation: " << message << std::endl;
    };
    config.logInstanceInfo = false;
    // SPIR-V compiled by the Makefile; rebuilding it (make shaders) while the example runs reloads it
    config.shaderSearchPaths = {shaderDir};
    config.autoReloadShaders = !headless;

    VulkanApp app;
    TRY(app.init(config));
    VulkanContext& ctx = app.getContext();

    std::vector<Handle<Mesh>> meshes;
    for (const MeshData& data : buildScene()) {
        TRY_ASSIGN(auto mesh, Mesh::create(ctx, data.vertices, data.indices));
        meshes.push_back(std::move(mesh));
    }

    // The app (SceneApp in scene.zgal) builds its graph, SceneGraph, with the per-frame uniforms,
    // shadows and exposure computed from its state. Headless, the graph is given the extent its
    // surface-relative images follow, and the smoke test's clock advances a fixed step each frame.
    RenderGraph& graph = app.createGraph();
    if (headless) {
        graph.setSurfaceExtent(options.size);
    }
    TRY_ASSIGN(auto sceneApp, scene::SceneApp::create(ctx, graph, scene::SceneApp::Inputs{
        .timestep = options.frames ? zgal::Timestep::fixed(1.0f / 60.0f) : zgal::Timestep::realTime(0.25f),
        .headless = headless,
        .capture = options.capture.has_value(),
        .meshes = std::move(meshes),
    }));
    app.setActiveGraph(graph);

    if (!headless) {
        std::cout << "Controls:\n"
                     "  drag (left mouse)  orbit the camera\n"
                     "  scroll             zoom\n"
                     "  L                  toggle the light's animation\n"
                     "  S                  toggle shadows\n"
                     "  + / -              exposure\n"
                     "  Esc                quit\n"
                     "Shaders reload when their SPIR-V changes: edit examples/scene/scene.zgal and run `make shaders`."
                  << std::endl;
    }
    if (!headless && !options.frames) {
        TRY(zgal::run(app, *sceneApp));
    } else {
        if (!headless) {
            zgal::connect(app, *sceneApp);
            app.addSurfaceResizeListener([](VkExtent2D extent) {
                std::cout << "Surface resized to " << extent.width << "x" << extent.height << std::endl;
            });
        }
        // The smoke test: shadows off for a quarter of the frames (the shadow pass drops out of the
        // plan), and the view resized halfway (and back, headless)
        const uint64_t frames = *options.frames;
        const VkExtent2D smaller{options.size.width * 3 / 4, options.size.height * 3 / 4};
        auto setShadows = [&](bool enabled) {
            scene::SceneApp::State state = sceneApp->state();
            state.shadows = enabled;
            sceneApp->setState(state);
        };
        for (uint64_t frame = 0; frame < frames; ++frame) {
            if (frame == frames / 4) {
                setShadows(false);
            } else if (frame == frames / 2) {
                setShadows(true);
                if (headless) {
                    graph.setSurfaceExtent(smaller);
                } else {
                    app.setWindowSize(smaller.width, smaller.height);
                }
            } else if (frame == 3 * frames / 4 && headless) {
                graph.setSurfaceExtent(options.size);
            }
            if (!app.pollEvents() || sceneApp->quitRequested()) {
                break;
            }
            TRY(zgal::drawFrame(app, *sceneApp));
        }
    }
    // Everything below the app is destroyed first, so the GPU must be done with it
    VK_TRY(ctx.waitIdleAll(), "Failed to wait for the device");

    if (sceneApp->ids().nodes.readback) {
        TRY_ASSIGN(auto pixels, graph.nodeAs<ReadbackNode>(*sceneApp->ids().nodes.readback).pixels());
        return std::optional(std::move(pixels));
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

    // Atomic: validation may report from any thread that calls Vulkan
    std::atomic<uint32_t> validationMessages = 0;
    auto captured = run(options, validationMessages);
    if (!captured) {
        std::cerr << "Error: " << captured.error() << std::endl;
        return EXIT_FAILURE;
    }
    // Any validation message is a bug
    if (validationMessages > 0) {
        std::cerr << validationMessages << " validation message(s)" << std::endl;
        return EXIT_FAILURE;
    }
    if (auto& pixels = *captured) {
        const VkExtent2D extent = options.size;
        if (!stbi_write_png(options.capture->c_str(), static_cast<int>(extent.width), static_cast<int>(extent.height), 4,
                            pixels->data(), static_cast<int>(extent.width * 4))) {
            std::cerr << "Failed to write " << *options.capture << std::endl;
            return EXIT_FAILURE;
        }
        std::cout << "Wrote " << *options.capture << std::endl;
    }
    return EXIT_SUCCESS;
}
