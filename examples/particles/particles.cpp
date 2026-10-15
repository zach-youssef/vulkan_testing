// GPU particles: a compute pass integrates 100k particles attracted to the mouse, and a graphics
// pass draws them with additive blending. The render graph orders the two, puts the compute pass
// on another queue when it can and synchronizes them; there are no barriers or semaphores here.
//
//     particles                                   interactive, in a window
//     particles --frames N [--capture out.png]    N frames headless (the smoke test)

#define VK_WRAP_UTIL_IMPL

#include "app/VulkanApp.h"
#include "descriptors/DescriptorHeap.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/DispatchNode.h"
#include "graph/nodes/ReadbackNode.h"
#include "materials/GraphicsMaterial.h"
#include "resources/Buffer.h"

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

// Defined by the Makefile
#ifndef PARTICLES_SHADER_DIR
#error "PARTICLES_SHADER_DIR must be defined to the directory containing the compiled shaders"
#endif

constexpr uint32_t kParticleCount = 100'000;
// The size of the headless image
constexpr VkExtent2D kHeadlessExtent{1024, 768};

// Matches Particle in the shaders (std430)
struct Particle {
    glm::vec2 position;
    glm::vec2 velocity;
    float life;
    float maxLife;
    glm::vec2 padding;
};
static_assert(sizeof(Particle) == 32);

// The simulation's parameters for one frame. Matches SimParams in the shaders (std430).
struct SimParams {
    glm::vec2 attractor;
    float dt;
    float time;
    float aspect;
    float attractorStrength;
    uint32_t step;
    uint32_t reset;
    uint32_t particleCount;
    float particleRadius;
};

// What the user (or the smoke test) asks of the simulation. Written by input callbacks between
// frames and read when the simulation material is updated.
struct Controls {
    bool paused = false;
    bool resetRequested = true;            // The particle buffer starts out uninitialized
    std::optional<glm::vec2> mouse;        // In simulation space, while the left button is held
    std::optional<float> fixedTimestep;    // Headless runs are deterministic
    float aspect = 1.0f;                   // Width / height of what we render to
};

// Integrates the particles (particles.comp). Owns the per-frame parameter buffers, which it fills
// in update(); the drawing material reads them too.
class ParticleSimMaterial : public ComputeMaterial {
public:
    ParticleSimMaterial(VulkanContext& context, Controls& controls, FrameBinding<DescriptorEntry> particles)
    : ComputeMaterial(context, {}, context.shaders().get("particles.comp")), controls_(&controls) {
        // A parameter buffer per frame in flight, so the CPU can write the next frame's while the
        // GPU reads the previous one's; then their descriptor entries and persistent mappings. The
        // collections are resized in this order when the number of frames in flight changes.
        paramBuffers_ = context.resources().create<PerFrameHandles<Buffer<SimParams>>>(
            context, [ctx = &context](uint32_t) {
                Handle<Buffer<SimParams>> buffer;
                VK_SUCCESS_OR_THROW(Buffer<SimParams>::create(buffer, *ctx, 1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                                              | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                                    "Failed to create simulation parameters");
                return buffer;
            });
        paramEntries_ = context.resources().create<PerFrameHandles<DescriptorEntry>>(
            context, [ctx = &context, buffers = paramBuffers_](uint32_t frame) {
                return ctx->descriptors().storageBuffer((*buffers)[frame]);
            });
        mappedParams_ = PerFrame<Mapping>(context, [this](uint32_t frame) {
            return (*paramBuffers_)[frame]->getPersistentMapping(0, sizeof(SimParams));
        });

        // Push constant order: params, particles. The particle binding tells the graph this node
        // reads and writes the particle buffer, which orders it before the draw.
        bindings_ = {
            MaterialBinding::storageBuffer(paramEntries_, Access::StorageBufferRead),
            MaterialBinding::storageBuffer(std::move(particles), Access::StorageBufferReadWrite),
        };
    }

    FrameBinding<DescriptorEntry> paramEntries() {
        return paramEntries_;
    }

    // Called by the graph once per frame, after the frame slot's previous use has finished, so its
    // parameter buffer is free to overwrite
    void update(uint32_t frameIndex, VkExtent2D surfaceExtent) override {
        const auto now = std::chrono::steady_clock::now();
        // Real time, clamped so a stall (e.g. dragging the window) doesn't fling particles away
        float dt = controls_->fixedTimestep.value_or(
            std::min(std::chrono::duration<float>(now - lastUpdate_).count(), 1.0f / 30.0f));
        lastUpdate_ = now;
        if (controls_->paused || step_ == 0) {
            dt = 0.0f;
        }
        time_ += dt;

        if (surfaceExtent.width > 0 && surfaceExtent.height > 0) {
            controls_->aspect = float(surfaceExtent.width) / float(surfaceExtent.height);
        }

        SimParams params{};
        // Without the mouse, the attractor wanders on a Lissajous curve
        params.attractor = controls_->mouse.value_or(
            glm::vec2{0.6f * controls_->aspect * std::sin(0.4f * time_), 0.5f * std::sin(0.7f * time_)});
        params.dt = dt;
        params.time = time_;
        params.aspect = controls_->aspect;
        params.attractorStrength = 0.15f;
        params.step = step_++;
        params.reset = std::exchange(controls_->resetRequested, false) ? 1u : 0u;
        params.particleCount = kParticleCount;
        params.particleRadius = 0.006f;
        memcpy(mappedParams_[frameIndex].get(), &params, sizeof(params));
    }

    glm::vec3 getDispatchDimensions() override {
        // 256 matches local_size_x in particles.comp
        return glm::vec3{(kParticleCount + 255) / 256, 1, 1};
    }

private:
    using Mapping = std::unique_ptr<SimParams, std::function<void(SimParams*)>>;

    Controls* controls_;
    Handle<PerFrameHandles<Buffer<SimParams>>> paramBuffers_;
    Handle<PerFrameHandles<DescriptorEntry>> paramEntries_;
    PerFrame<Mapping> mappedParams_;
    std::chrono::steady_clock::time_point lastUpdate_ = std::chrono::steady_clock::now();
    float time_ = 0.0f;
    uint32_t step_ = 0;
};

// Draws every particle as a soft quad with additive blending (particles.vert, particles.frag).
//
// The vertex shader pulls particles from the storage buffer by gl_VertexIndex instead of reading
// them as vertex attributes. That's the bindless way: the buffer is already in the universal
// descriptor set for the compute pass, it needs no vertex input state, and the graph sees the read
// through the material's binding like any other. (A vertex buffer would need instancing for quads
// and a separate VertexRead declaration.)
class ParticleDrawMaterial : public GraphicsMaterial {
public:
    ParticleDrawMaterial(VulkanContext& context, AttachmentFormats formats,
                         FrameBinding<DescriptorEntry> params, FrameBinding<DescriptorEntry> particles)
    : GraphicsMaterial(context, {
          MaterialBinding::storageBuffer(std::move(params), Access::StorageBufferRead, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT),
          MaterialBinding::storageBuffer(std::move(particles), Access::StorageBufferRead, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT),
      }, std::move(formats), context.shaders().get("particles.vert"), context.shaders().get("particles.frag"),
      GraphicsPipelineOptions{
          .vertexLayout = std::nullopt,
          .cullMode = VK_CULL_MODE_NONE,
          // Overlapping particles add up, so the result doesn't depend on draw order and needs no
          // depth buffer
          .blend = {BlendMode::Additive},
      }) {}

    void draw(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
        bind(commandBuffer, frameIndex);
        vkCmdDraw(commandBuffer, 6 * kParticleCount, 1, 0, 0);
    }
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

int run(const Options& options, std::atomic<uint32_t>& validationMessages) {
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
    app.init(config);
    VulkanContext& context = app.getContext();

    Controls controls;
    RenderGraph& graph = app.createGraph();

    // What we draw into: the window, or (headless) an image we read back. Without depth, since
    // additive blending doesn't need it.
    RenderTarget target;
    ImageId headlessImage;
    const std::array<float, 4> background{0.0f, 0.0f, 0.01f, 1.0f};
    if (headless) {
        headlessImage = graph.createImage("frame", FrameMode::PerFrame, VK_FORMAT_R8G8B8A8_SRGB,
                                          FixedExtent{kHeadlessExtent});
        target.color.push_back({headlessImage, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, background});
        controls.aspect = float(kHeadlessExtent.width) / float(kHeadlessExtent.height);
        controls.fixedTimestep = 1.0f / 60.0f;
    } else {
        target = graph.surfaceTarget(background, std::nullopt);
    }

    // The particles live on the GPU only, in one buffer shared by every frame: each frame's
    // simulation continues from the last. The graph derives its usage from the uses below, and
    // makes a frame's compute pass wait for the previous frame's draw.
    BufferId particles = graph.createBuffer("particles", FrameMode::Shared, kParticleCount * sizeof(Particle));

    auto simulation = context.resources().create<ParticleSimMaterial>(context, controls,
                                                                       graph.storageBufferEntry(particles));
    auto drawing = context.resources().create<ParticleDrawMaterial>(context, graph.formats(target),
                                                                     simulation->paramEntries(),
                                                                     graph.storageBufferEntry(particles));

    // Nodes, in program order. The simulation writes the buffer and the draw reads it, so the graph
    // runs them in that order (on different queues if it can) and adds the synchronization.
    graph.add<DispatchNode>("simulate", simulation);
    graph.addGraphics("draw particles",
                      [&](NodeBuilder& builder) { builder.renderTo(target).uses(drawing); },
                      [drawing](RecordContext& record) { drawing->draw(record.commandBuffer, record.frameIndex); });

    // Prints the frame rate every couple of seconds. CPU nodes run each frame in graph order.
    auto lastReport = std::chrono::steady_clock::now();
    uint64_t lastReportFrame = 0;
    graph.addCpu("report", nullptr, [&](FrameContext& frame) {
        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - lastReport).count();
        if (seconds >= 2.0f) {
            std::cout << float(frame.frameNumber - lastReportFrame) / seconds << " fps, "
                      << frame.context.framesInFlight() << " frames in flight" << std::endl;
            lastReport = now;
            lastReportFrame = frame.frameNumber;
        }
    });

    NodeId readback;
    if (headless) {
        readback = graph.add<ReadbackNode>("readback", headlessImage);
    }
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
            app.drawFrame();
        }
        if (options.capturePath && frames > 0) {
            auto pixels = graph.nodeAs<ReadbackNode>(readback).pixels();
            if (!stbi_write_png(options.capturePath->c_str(), int(kHeadlessExtent.width), int(kHeadlessExtent.height),
                                4, pixels.data(), int(kHeadlessExtent.width * 4))) {
                throw std::runtime_error("Failed to write " + *options.capturePath);
            }
            std::cout << "Wrote " << *options.capturePath << std::endl;
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
        app.run();
    }
    return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
    std::atomic<uint32_t> validationMessages = 0;
    int result = EXIT_FAILURE;
    try {
        // Everything is destroyed when run() returns, so destruction is validated too
        result = run(parseOptions(argc, argv), validationMessages);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }
    if (validationMessages > 0) {
        std::cerr << validationMessages << " validation message(s)" << std::endl;
        return EXIT_FAILURE;
    }
    return result;
}
