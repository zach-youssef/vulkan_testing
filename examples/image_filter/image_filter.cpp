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
#include "graph/RenderGraph.h"
#include "graph/nodes/ReadbackNode.h"
#include "resources/Image.h"

// Generated from image_filter.zgal, in namespace image_filter: the filters' materials, FilterGraph
// and FilterApp
#include "image_filter.zgal.h"

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

// RGBA8 pixels, tightly packed
struct FilteredImage {
    std::vector<uint8_t> pixels;
    VkExtent2D extent;
};

// Builds the graph and runs it once. Every handle created here is released before returning, so
// before the context is destroyed.
Result<FilteredImage> filterImage(VulkanContext& context, const Options& options) {
    TRY_ASSIGN(auto source, Image::createFromFile(context, options.input, ColorSpace::Srgb));
    const VkExtent2D extent{source->getWidth(), source->getHeight()};

    // The app, FilterApp in image_filter.zgal, builds its graph, FilterGraph: every pass, with the
    // ones the user didn't ask for disabled each frame. Disabled passes still need valid
    // parameters: the radius is only used if the blur runs.
    RenderGraph graph(context);
    TRY_ASSIGN(auto filter, image_filter::FilterApp::create(context, graph, image_filter::FilterApp::Inputs{
        .blur = options.blurRadius.has_value(),
        .radius = options.blurRadius.value_or(1),
        .edges = options.edges,
        .binarize = options.threshold.has_value(),
        .level = options.threshold.value_or(0.5f),
        .source = source,
    }));
    // Without a VulkanApp, the frame begins here: the app enables the passes its state asks for
    TRY(filter->beginFrame());

    // execute() would compile on demand; compiling first reports graph errors before any GPU work
    TRY(graph.compile().withContext("compiling the filter graph"));
    if (options.printPlan) {
        TRY_ASSIGN(const std::string plan, graph.dumpPlan());
        std::cout << plan << std::endl;
    }

    TRY(graph.execute());
    // Waits for the frame to finish. The readback is RGBA8, tightly packed.
    TRY_ASSIGN(auto pixels, graph.nodeAs<ReadbackNode>(filter->ids().nodes.readback).pixels());
    return FilteredImage{std::move(pixels), extent};
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
    std::optional<FilteredImage> image;
    {
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
        auto context = VulkanContext::create(std::move(config));
        if (!context) {
            std::cerr << "image_filter: " << context.error() << std::endl;
            return 1;
        }
        if (options.validation && !(*context)->validationEnabled()) {
            std::cerr << "image_filter: --validation was requested, but the validation layer is unavailable" << std::endl;
            return 1;
        }
        auto filtered = filterImage(**context, options);
        if (!filtered) {
            std::cerr << "image_filter: " << filtered.error() << std::endl;
            return 1;
        }
        image = std::move(*filtered);
    }

    // Counted after the context is gone, so messages about leaks at destruction count too
    if (validationMessages > 0) {
        std::cerr << "image_filter: " << validationMessages << " validation message(s)" << std::endl;
        return 1;
    }

    const VkExtent2D extent = image->extent;
    if (!stbi_write_png(options.output.c_str(), static_cast<int>(extent.width), static_cast<int>(extent.height), 4,
                        image->pixels.data(), static_cast<int>(extent.width * 4))) {
        std::cerr << "image_filter: failed to write " << options.output << std::endl;
        return 1;
    }
    return 0;
}
