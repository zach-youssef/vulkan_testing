// A lit 3D scene: the example of a "normal" forward renderer. Each frame runs three passes,
//   1. shadow map: the scene's depth from a directional light, into a fixed-size depth image
//   2. lit:        PBR shading with filtered shadows into a linear HDR image the size of the window
//   3. tonemap:    exposure and an ACES curve from the HDR image to the window (or, headless, to an
//                  image read back into a PNG)
// and the render graph derives their order, barriers and queues from what each pass uses. The
// shaders' interfaces and the graph are declared in ZGAL (scene.zgal); this file supplies the
// meshes and per-frame data, and handles input.
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
// Generated from scene.zgal: SceneUniforms, SceneVertex, the Shadow, Lit and Tonemap materials and
// SceneGraph
#include "scene.zgal.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
// For --capture
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "util/StbImageWrite.h"

#include <atomic>
#include <chrono>
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
// Scene state: what the user controls, read by the materials every frame
// ---------------------------------------------------------------------------------------------------

// GLM's projections are for OpenGL, whose clip space Y points up; Vulkan's points down
glm::mat4 flipY(glm::mat4 projection) {
    projection[1][1] *= -1.0f;
    return projection;
}

struct OrbitCamera {
    glm::vec3 target{0.0f, 0.75f, 0.0f};
    float yaw = glm::radians(30.0f);     // Around +Y, from +Z
    float pitch = glm::radians(25.0f);   // Above the horizon
    float distance = 11.0f;

    glm::vec3 position() const {
        return target + distance * glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                             std::cos(pitch) * std::cos(yaw));
    }

    glm::mat4 viewProjection(float aspect) const {
        const glm::mat4 view = glm::lookAt(position(), target, glm::vec3(0.0f, 1.0f, 0.0f));
        return flipY(glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f)) * view;
    }

    void rotate(float dx, float dy) {
        yaw -= dx * 0.008f;
        pitch = glm::clamp(pitch + dy * 0.008f, glm::radians(2.0f), glm::radians(85.0f));
    }

    void zoom(float steps) {
        distance = glm::clamp(distance * std::pow(0.9f, steps), 3.0f, 40.0f);
    }
};

// A sun: parallel rays, so its shadow map uses an orthographic projection covering the scene
struct SunLight {
    float azimuth = glm::radians(-60.0f);
    float elevation = glm::radians(50.0f);
    bool animated = true;
    // Half the size of the area the shadow map covers: tighter is sharper
    static constexpr float kSceneRadius = 7.0f;

    // Unit vector from the scene towards the light
    glm::vec3 direction() const {
        return {std::cos(elevation) * std::sin(azimuth), std::sin(elevation), std::cos(elevation) * std::cos(azimuth)};
    }

    glm::mat4 viewProjection() const {
        const glm::mat4 view = glm::lookAt(direction() * 2.0f * kSceneRadius, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const float r = kSceneRadius;
        return flipY(glm::ortho(-r, r, -r, r, 0.1f, 4.0f * r)) * view;
    }
};

struct SceneState {
    OrbitCamera camera;
    SunLight light;
    bool shadows = true;
    float exposure = 1.0f;
};

// ---------------------------------------------------------------------------------------------------
// Per-frame data
// ---------------------------------------------------------------------------------------------------

// The frame's SceneUniforms (scene.zgal), which the graph writes into the frame's uniform buffer
// for the shadow and lit passes
SceneUniforms sceneUniforms(const SceneState& state, const zgal::FrameInfo& frame) {
    // The HDR image is the surface's size (headless, the extent given to the graph)
    const float aspect = static_cast<float>(frame.surfaceExtent.width) / static_cast<float>(frame.surfaceExtent.height);
    // Designated, as the generated struct has padding members between its vec3s
    return SceneUniforms{
        .viewProjection = state.camera.viewProjection(aspect),
        .lightViewProjection = state.light.viewProjection(),
        .cameraPosition = state.camera.position(),
        .lightDirection = state.light.direction(),
        .lightRadiance = glm::vec3(1.0f, 0.95f, 0.85f) * 3.0f,
        .skyAmbient = glm::vec3(0.25f, 0.32f, 0.45f),
        .groundAmbient = glm::vec3(0.10f, 0.09f, 0.08f),
    };
}

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

// ---------------------------------------------------------------------------------------------------
// Input (windowed only)
// ---------------------------------------------------------------------------------------------------

void setShadows(SceneState& state, RenderGraph& graph, NodeId shadowPass, bool enabled) {
    // Disabling the pass drops it from the plan; the push constant stops the lit pass sampling the
    // map it no longer writes
    state.shadows = enabled;
    graph.setEnabled(shadowPass, enabled);
}

// Key presses, received from VulkanApp::runFrame's event polling, between frames
void onKey(GLFWwindow* window, int key, int action, SceneState& state, RenderGraph& graph, NodeId shadowPass) {
    if (action != GLFW_PRESS) {
        return;
    }
    switch (key) {
        case GLFW_KEY_L: state.light.animated = !state.light.animated; break;
        case GLFW_KEY_S: setShadows(state, graph, shadowPass, !state.shadows); break;
        case GLFW_KEY_EQUAL: case GLFW_KEY_KP_ADD: state.exposure *= 1.25f; break;
        case GLFW_KEY_MINUS: case GLFW_KEY_KP_SUBTRACT: state.exposure /= 1.25f; break;
        case GLFW_KEY_ESCAPE: glfwSetWindowShouldClose(window, GLFW_TRUE); break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------------------------------

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

    SceneState state;

    // The meshes, which the shadow and lit passes both draw, sharing their buffers
    std::vector<Handle<Mesh>> meshes;
    for (const MeshData& data : buildScene()) {
        TRY_ASSIGN(auto mesh, Mesh::create(ctx, data.vertices, data.indices));
        meshes.push_back(std::move(mesh));
    }

    // The graph, SceneGraph in scene.zgal: its samplers, images, materials and passes. The HDR
    // image and its depth follow the window's size (the graph recreates them on resize); headless,
    // they follow the extent given to the graph, which works the same way. The inputs read the
    // state each frame.
    RenderGraph& graph = app.createGraph();
    if (headless) {
        graph.setSurfaceExtent(options.size);
    }
    TRY_ASSIGN(const SceneGraph scene, SceneGraph::build(ctx, graph, SceneGraph::Inputs{
        .headless = headless,
        .capture = options.capture.has_value(),
        .meshes = std::move(meshes),
        .uniforms = [&state](const zgal::FrameInfo& frame) { return sceneUniforms(state, frame); },
        .shadows = [&state](const zgal::FrameInfo&) { return state.shadows; },
        .exposure = [&state](const zgal::FrameInfo&) { return state.exposure; },
    }));
    const NodeId shadowPass = scene.nodes.shadowPass;
    app.setActiveGraph(graph);

    GLFWwindow* window = nullptr;
    if (!headless) {
        // The window is polled for dragging each frame; keys and scrolling arrive as events
        window = app.window();
        app.setInputHandlers(InputHandlers{
            .key = [&, window](int key, int, int action, int) { onKey(window, key, action, state, graph, shadowPass); },
            .scroll = [&state](double, double yOffset) { state.camera.zoom(static_cast<float>(yOffset)); },
        });
        std::cout << "Controls:\n"
                     "  drag (left mouse)  orbit the camera\n"
                     "  scroll             zoom\n"
                     "  L                  toggle the light's animation\n"
                     "  S                  toggle shadows\n"
                     "  + / -              exposure\n"
                     "  Esc                quit\n"
                     "Shaders reload when their SPIR-V changes: edit examples/scene/shaders and run `make shaders`."
                  << std::endl;
    }

    if (window && options.frames) {
        // Reports the smoke test's resize; the graph itself needs no listener
        app.addSurfaceResizeListener([](VkExtent2D extent) {
            std::cout << "Surface resized to " << extent.width << "x" << extent.height << std::endl;
        });
    }

    double lastX = 0.0, lastY = 0.0;
    if (window) {
        glfwGetCursorPos(window, &lastX, &lastY);
    }
    auto lastTime = std::chrono::steady_clock::now();
    for (uint64_t frame = 0; !options.frames || frame < *options.frames; ++frame) {
        // Smoke tests use a fixed time step, so captures are reproducible
        const auto now = std::chrono::steady_clock::now();
        const float dt = options.frames ? 1.0f / 60.0f : std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        if (state.light.animated) {
            state.light.azimuth += 0.4f * dt;
        }

        if (window) {
            double x = 0.0, y = 0.0;
            glfwGetCursorPos(window, &x, &y);
            if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
                state.camera.rotate(static_cast<float>(x - lastX), static_cast<float>(y - lastY));
            }
            lastX = x;
            lastY = y;
        }

        if (options.frames) {
            // Smoke tests exercise what a user would: shadows off for a while (a different plan),
            // and a resize (the graph recreates the surface-sized images). Headless, the size is
            // restored afterwards so the capture has the requested size.
            const uint64_t frames = *options.frames;
            const VkExtent2D smaller{options.size.width * 3 / 4, options.size.height * 3 / 4};
            if (frame == frames / 4) {
                setShadows(state, graph, shadowPass, false);
            } else if (frame == frames / 2) {
                setShadows(state, graph, shadowPass, true);
                if (window) {
                    app.setWindowSize(smaller.width, smaller.height);
                } else {
                    graph.setSurfaceExtent(smaller);
                }
            } else if (frame == 3 * frames / 4 && !window) {
                graph.setSurfaceExtent(options.size);
            }
        }

        TRY_ASSIGN(const FrameOutcome outcome, app.runFrame());
        if (outcome == FrameOutcome::WindowClosed) {
            break;
        }
    }
    // Everything below the app is destroyed first, so the GPU must be done with it
    VK_TRY(ctx.waitIdleAll(), "Failed to wait for the device");

    if (scene.nodes.readback) {
        TRY_ASSIGN(auto pixels, graph.nodeAs<ReadbackNode>(*scene.nodes.readback).pixels());
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
