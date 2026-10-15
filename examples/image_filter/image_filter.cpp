/*
 image_filter: a headless command-line GPU image filter, and the example of using the wrapper
 without a window (a VulkanContext and a RenderGraph, no VulkanApp or GLFW).

    image_filter <in.png|jpg> <out.png> [--blur <radius>] [--edges] [--threshold <0..1>]
                 [--validation] [--plan]

 Every filter is a compute pass (a DispatchNode) over graph-owned images:

    input (sRGB) -load-> working -blur rows-> scratch -blur columns-> working
                         working -sobel rows-> scratch -sobel columns-> working
                         working -threshold-> working (in place)
                         working -store-> encoded (sRGB bytes) -readback-> PNG

 Each filter takes the working image back to the working image, so the graph contains every pass
 and the ones the user didn't ask for are disabled. Their uses still count when the graph sizes
 and allocates its images, so toggling passes never reallocates, and plans are cached per set of
 enabled nodes: a long-running tool (e.g. an interactive editor) could flip filters between
 frames and only compile each combination once.

 Colour: the input is an sRGB texture, so sampling it decodes to linear colour. The working images
 hold that linear colour as half floats (8 bits would band in linear space), and blurring happens
 in linear light. Storage images can't be sRGB, so the store pass encodes sRGB itself into a UNORM
 image. Without filters the output therefore matches the input.
 */
#define VK_WRAP_UTIL_IMPL

#include "core/VulkanContext.h"
#include "descriptors/DescriptorHeap.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/DispatchNode.h"
#include "graph/nodes/ReadbackNode.h"
#include "materials/Material.h"
#include "resources/Image.h"
#include "shaders/ShaderLibrary.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "util/StbImageWrite.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

// Defined by the Makefile
#ifndef IMAGE_FILTER_SHADER_DIR
#error "IMAGE_FILTER_SHADER_DIR must be defined to the directory containing the compiled shaders"
#endif

namespace {

const char* kUsage =
    "usage: image_filter <in.png|jpg> <out.png> [--blur <radius>] [--edges] [--threshold <0..1>]\n"
    "                    [--validation] [--plan]\n"
    "  --blur <radius>     Gaussian blur, radius in pixels (1-64)\n"
    "  --edges             Sobel edge magnitude, as grey\n"
    "  --threshold <t>     black or white by perceived brightness (0.5 = mid grey)\n"
    "  --validation        enable Vulkan validation; any validation message fails the run\n"
    "  --plan              print the render graph's plan\n"
    "Filters run in the order blur, edges, threshold. Without any, the image is copied.";

constexpr int kMaxBlurRadius = 64;

struct Options {
    std::string input;
    std::string output;
    std::optional<int> blurRadius;
    bool edges = false;
    std::optional<float> threshold;
    bool validation = false;
    bool printPlan = false;
};

// Throws std::invalid_argument with a message for the user
Options parseArguments(int argc, char** argv) {
    Options options;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(arg + " needs a value");
            }
            return argv[++i];
        };
        auto number = [&](const std::string& text) {
            size_t used = 0;
            double parsed = 0.0;
            try {
                parsed = std::stod(text, &used);
            } catch (const std::exception&) {
            }
            if (used == 0 || used != text.size()) {
                throw std::invalid_argument(arg + " expects a number, not '" + text + "'");
            }
            return parsed;
        };

        if (arg == "--blur") {
            const double radius = number(value());
            if (radius != static_cast<int>(radius) || radius < 1 || radius > kMaxBlurRadius) {
                throw std::invalid_argument("--blur radius must be a whole number from 1 to " + std::to_string(kMaxBlurRadius));
            }
            options.blurRadius = static_cast<int>(radius);
        } else if (arg == "--edges") {
            options.edges = true;
        } else if (arg == "--threshold") {
            const double threshold = number(value());
            if (threshold < 0.0 || threshold > 1.0) {
                throw std::invalid_argument("--threshold must be between 0 and 1");
            }
            options.threshold = static_cast<float>(threshold);
        } else if (arg == "--validation") {
            options.validation = true;
        } else if (arg == "--plan") {
            options.printPlan = true;
        } else if (arg == "-h" || arg == "--help") {
            throw std::invalid_argument("");
        } else if (arg.starts_with("--")) {
            throw std::invalid_argument("unknown option " + arg);
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() != 2) {
        throw std::invalid_argument("expected an input and an output image");
    }
    options.input = positional[0];
    options.output = positional[1];
    return options;
}

// Push constant data for a filter's parameters. Shaders declare them after the binding indices,
// so keep members 4-byte scalars: they then have the same offsets in C++ and GLSL.
template<typename Params>
std::vector<uint8_t> pushData(const Params& params) {
    static_assert(std::is_trivially_copyable_v<Params>);
    std::vector<uint8_t> data(sizeof(Params));
    memcpy(data.data(), &params, sizeof(Params));
    return data;
}

struct BlurParams {
    int32_t directionX;
    int32_t directionY;
    int32_t radius;
    float sigma;
};

struct ThresholdParams {
    float threshold;
};

// One filter pass: a compute shader run once per pixel, reading and writing the resources of its
// bindings, with the filter's parameters pushed after the bindings' indices
class FilterMaterial : public ComputeMaterial {
public:
    FilterMaterial(VulkanContext& context, Handle<Shader> shader, std::vector<MaterialBinding> bindings,
                   VkExtent2D extent, std::vector<uint8_t> params)
    : ComputeMaterial(context, std::move(bindings), std::move(shader)), extent_(extent), params_(std::move(params)) {}

    void update(uint32_t, VkExtent2D) override {
        // The parameters never change, so there's nothing to update per frame
    }

    glm::vec3 getDispatchDimensions() override {
        // 16 x 16 matches the shaders' workgroup size (filter_common.glsl). Round up so images
        // that aren't a multiple of it are covered; the shaders skip the excess invocations.
        return {(extent_.width + 15) / 16, (extent_.height + 15) / 16, 1};
    }

protected:
    void writePushConstants(uint32_t, std::vector<uint8_t>& data) override {
        data.insert(data.end(), params_.begin(), params_.end());
    }

private:
    VkExtent2D extent_;
    std::vector<uint8_t> params_;
};

// Builds the graph, runs it once and writes the result. Every handle created here is released
// before returning, so before the context is destroyed.
void filterImage(VulkanContext& context, const Options& options) {
    Handle<Image> source;
    Image::createFromFile(source, context, options.input);
    const VkExtent2D extent{source->getWidth(), source->getHeight()};
    Handle<VulkanSampler> sampler;
    VK_SUCCESS_OR_THROW(VulkanSampler::createWithModeAndFilter(sampler, context, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                                               VK_FILTER_NEAREST),
                        "Failed to create sampler");

    // The graph's images. They're only used within one frame, so one copy is shared by every frame
    // in flight rather than one per frame.
    RenderGraph graph(context);
    // createFromFile leaves the input ready for sampling, and the graph keeps it that way
    graph.importImage("input", source, ImageState::shaderReadOnly());
    ImageId working = graph.createImage("working", FrameMode::Shared, VK_FORMAT_R16G16B16A16_SFLOAT, FixedExtent{extent});
    ImageId scratch = graph.createImage("scratch", FrameMode::Shared, VK_FORMAT_R16G16B16A16_SFLOAT, FixedExtent{extent});
    ImageId encoded = graph.createImage("encoded", FrameMode::Shared, VK_FORMAT_R8G8B8A8_UNORM, FixedExtent{extent});

    // Bindings to graph images. Their access is what the graph synchronizes on: e.g. the blur's
    // column pass reading scratch waits for the row pass writing it.
    auto reads = [&](ImageId image) {
        return MaterialBinding::storageImage(graph.storageEntry(image), Access::StorageRead);
    };
    auto writes = [&](ImageId image) {
        return MaterialBinding::storageImage(graph.storageEntry(image), Access::StorageWrite);
    };
    // Adds a pass. DispatchNode declares the material's uses, so nothing else needs declaring.
    auto addPass = [&](std::string name, const std::string& shader, std::vector<MaterialBinding> bindings,
                       std::vector<uint8_t> params = {}) {
        auto material = context.resources().create<FilterMaterial>(context, context.shaders().get(shader),
                                                                   std::move(bindings), extent, std::move(params));
        return graph.add<DispatchNode>(std::move(name), std::move(material));
    };

    // Insertion order is program order wherever passes use the same image
    auto& heap = context.descriptors();
    addPass("load", "load.comp", {MaterialBinding::sampled(heap.sampledImage(source)),
                                  MaterialBinding::sampler(heap.sampler(sampler)), writes(working)});

    // Disabled passes still need valid parameters: the radius is only used if the blur runs
    const int radius = options.blurRadius.value_or(1);
    const float sigma = radius / 2.0f;   // The kernel spans two standard deviations either side
    const NodeId blurRows = addPass("blur rows", "blur.comp", {reads(working), writes(scratch)},
                                    pushData(BlurParams{1, 0, radius, sigma}));
    const NodeId blurColumns = addPass("blur columns", "blur.comp", {reads(scratch), writes(working)},
                                       pushData(BlurParams{0, 1, radius, sigma}));

    const NodeId sobelRows = addPass("sobel rows", "sobel_rows.comp", {reads(working), writes(scratch)});
    const NodeId sobelColumns = addPass("sobel columns", "sobel_columns.comp", {reads(scratch), writes(working)});

    const NodeId threshold = addPass("threshold", "threshold.comp",
                                     {MaterialBinding::storageImage(graph.storageEntry(working), Access::StorageReadWrite)},
                                     pushData(ThresholdParams{options.threshold.value_or(0.5f)}));

    addPass("store", "store.comp", {reads(working), writes(encoded)});
    const NodeId readback = graph.add<ReadbackNode>("readback", encoded);

    graph.setEnabled(blurRows, options.blurRadius.has_value());
    graph.setEnabled(blurColumns, options.blurRadius.has_value());
    graph.setEnabled(sobelRows, options.edges);
    graph.setEnabled(sobelColumns, options.edges);
    graph.setEnabled(threshold, options.threshold.has_value());

    // execute() would compile on demand; compiling first reports graph errors before any GPU work
    graph.compile();
    if (options.printPlan) {
        std::cout << graph.dumpPlan() << std::endl;
    }

    graph.execute();
    // Waits for the frame to finish. The readback is RGBA8, tightly packed.
    std::vector<uint8_t> pixels = graph.nodeAs<ReadbackNode>(readback).pixels();

    if (!stbi_write_png(options.output.c_str(), static_cast<int>(extent.width), static_cast<int>(extent.height), 4,
                        pixels.data(), static_cast<int>(extent.width * 4))) {
        throw std::runtime_error("Failed to write " + options.output);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        options = parseArguments(argc, argv);
    } catch (const std::invalid_argument& e) {
        if (*e.what()) {
            std::cerr << "image_filter: " << e.what() << "\n";
        }
        std::cerr << kUsage << std::endl;
        return 2;
    }

    // Validation messages arrive on whichever thread calls Vulkan, until the context is destroyed
    std::atomic<int> validationMessages = 0;
    try {
        VulkanContextConfig config;
        config.applicationName = "image_filter";
        config.shaderSearchPaths = {IMAGE_FILTER_SHADER_DIR};
        config.enableValidation = options.validation;
        config.enableSynchronizationValidation = options.validation;
        config.onValidationMessage = [&validationMessages](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                           const char* message) {
            ++validationMessages;
            const char* label = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning";
            std::cerr << "[Vulkan validation " << label << "] " << message << std::endl;
        };
        // No surface: the context is headless and needs no window system
        VulkanContext context(std::move(config));
        if (options.validation && !context.validationEnabled()) {
            std::cerr << "image_filter: --validation was requested, but the validation layer is unavailable" << std::endl;
            return 1;
        }
        filterImage(context, options);
    } catch (const std::exception& e) {
        std::cerr << "image_filter: " << e.what() << std::endl;
        return 1;
    }

    // Counted after the context is gone, so messages about leaks at destruction count too
    if (validationMessages > 0) {
        std::cerr << "image_filter: " << validationMessages << " validation message(s)" << std::endl;
        return 1;
    }
    return 0;
}
