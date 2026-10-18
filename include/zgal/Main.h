#pragma once

#include "core/Result.h"
#include "core/VulkanContext.h"
#include "graph/RenderGraph.h"
#include "graph/nodes/ReadbackNode.h"
#include "util/StbImageWrite.h"
#include "zgal/App.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

/*
 What the mains ZGAL generates (a file's `main`, `make docs`) use: the command line, with its
 built-in options, counting validation messages, writing captures, and running a headless program.
 zgal/Window.h runs programs with a window. Free of windowing, so headless programs don't need a
 window system.

 Exit statuses: 0, 1 for an error or validation messages (with --validation), 2 for a bad command
 line.
 */
namespace zgal {

// What a generated main declares
struct MainSpec {
    struct Window {
        std::string title;
        uint32_t width;
        uint32_t height;
    };
    std::string program;                   // Its name, for the usage
    std::string about;                     // What it does, for the usage (may be empty)
    std::optional<Window> window;          // Without one, the program is only headless
    bool captureOption = false;            // Whether it has --capture FILE
    std::vector<std::string> controls;     // Printed when it runs interactively
};

// What the built-in options chose
struct Launch {
    std::optional<uint32_t> frames;                 // --frames N, else until the window closes
    std::optional<std::filesystem::path> capture;   // Where the last frame's capture goes
    VkExtent2D size{};                              // --size, or the window's
    bool window = false;                            // Whether it runs in a window
    bool validation = false;                        // --validation
    bool plan = false;                              // --plan
    bool timings = false;                           // --timings

    // How the app is run (its headless, capture and frameLimit)
    RunMode mode() const {
        return window ? RunMode::window(frames.value_or(0)) : RunMode::headless(frames.value_or(0), capture.has_value());
    }

    // The app's clock: real time interactively, a fixed step for a number of frames, so those runs
    // are deterministic
    Timestep timestep(float maxStep, float fixedStep) const {
        return frames ? Timestep::fixed(fixedStep) : Timestep::realTime(maxStep);
    }
};

// Parses a number for an option, within [low, high] when given: an error message, or nullopt
template<typename T>
std::optional<std::string> parseNumber(const std::string& text, T& out, std::optional<T> low = std::nullopt,
                                       std::optional<T> high = std::nullopt) {
    T value{};
    bool parsed = false;
    if constexpr (std::is_floating_point_v<T>) {
        // from_chars for floating point isn't in every standard library yet
        char* stop = nullptr;
        const double d = text.empty() ? 0.0 : std::strtod(text.c_str(), &stop);
        parsed = !text.empty() && stop == text.c_str() + text.size() && std::isfinite(d);
        value = static_cast<T>(d);
    } else {
        const char* end = text.data() + text.size();
        const auto [stop, error] = std::from_chars(text.data(), end, value);
        parsed = !text.empty() && error == std::errc() && stop == end;
    }
    if (!parsed) {
        return "expects " + std::string(std::is_floating_point_v<T> ? "a number" : std::is_signed_v<T> ? "a whole number"
                                                                                                    : "a whole number of at least 0")
               + ", not '" + text + "'";
    }
    if ((low && value < *low) || (high && value > *high)) {
        std::ostringstream range;
        range << "must be from " << (low ? *low : value) << " to " << (high ? *high : value);
        return range.str();
    }
    out = value;
    return std::nullopt;
}

// A path given on the command line, which is relative to the working directory (asset search paths
// don't apply to it)
inline std::filesystem::path commandLinePath(const std::string& text) {
    return std::filesystem::absolute(text);
}

/*
 A program's command line: the built-in options, then the app's flags, options and positional
 arguments, which the generated main adds. Setters parse a value into the app's inputs, returning an
 error message or nullopt.
 */
class CommandLine {
public:
    using Setter = std::function<std::optional<std::string>(const std::string&)>;

    explicit CommandLine(MainSpec spec) : spec_(std::move(spec)) {}

    void flag(std::string name, std::string help, std::function<void()> set) {
        options_.push_back({std::move(name), "", std::move(help), [set = std::move(set)](const std::string&) {
                                set();
                                return std::optional<std::string>();
                            }, false});
    }

    void option(std::string name, std::string metavar, std::string help, Setter set) {
        options_.push_back({std::move(name), std::move(metavar), std::move(help), std::move(set), true});
    }

    void positional(std::string metavar, std::string help, Setter set) {
        positionals_.push_back({"", std::move(metavar), std::move(help), std::move(set), true});
    }

    // The command line's built-in choices, or (for a bad command line, or --help) the exit status,
    // having printed why or the usage
    std::variant<Launch, int> parse(int argc, char** argv) const {
        Launch launch;
        launch.window = spec_.window.has_value();
        if (spec_.window) {
            launch.size = {spec_.window->width, spec_.window->height};
        }
        bool forceWindow = false;
        size_t positional = 0;
        auto fail = [this](const std::string& message) {
            std::cerr << spec_.program << ": " << message << "\n" << usage() << std::flush;
            return 2;
        };
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::optional<std::string> {
                if (i + 1 >= argc) {
                    return std::nullopt;
                }
                return std::string(argv[++i]);
            };
            if (arg == "-h" || arg == "--help") {
                std::cout << usage() << std::flush;
                return 0;
            } else if (arg == "--validation") {
                launch.validation = true;
            } else if (arg == "--plan") {
                launch.plan = true;
            } else if (arg == "--timings") {
                launch.timings = true;
            } else if (arg == "--frames") {
                const auto v = value();
                uint32_t frames = 0;
                if (!v) {
                    return fail("--frames needs a value");
                }
                if (const auto error = parseNumber<uint32_t>(*v, frames, 1u)) {
                    return fail("--frames " + *error);
                }
                launch.frames = frames;
            } else if (arg == "--capture" && spec_.captureOption) {
                const auto v = value();
                if (!v) {
                    return fail("--capture needs a value");
                }
                launch.capture = commandLinePath(*v);
            } else if (arg == "--size" && spec_.window) {
                const auto v = value();
                const size_t x = v ? v->find('x') : std::string::npos;
                uint32_t width = 0, height = 0;
                if (x == std::string::npos || parseNumber<uint32_t>(v->substr(0, x), width, 1u)
                    || parseNumber<uint32_t>(v->substr(x + 1), height, 1u)) {
                    return fail("--size takes WIDTHxHEIGHT, e.g. 640x360");
                }
                launch.size = {width, height};
            } else if (arg == "--window" && spec_.window) {
                forceWindow = true;
            } else if (arg.starts_with("--")) {
                const Option* option = find(arg);
                if (!option) {
                    return fail("unknown option " + arg);
                }
                std::string text;
                if (option->takesValue) {
                    const auto v = value();
                    if (!v) {
                        return fail(arg + " needs a value");
                    }
                    text = *v;
                }
                if (const auto error = option->set(text)) {
                    return fail(arg + " " + *error);
                }
            } else {
                if (positional >= positionals_.size()) {
                    return fail("unexpected argument " + arg);
                }
                if (const auto error = positionals_[positional].set(arg)) {
                    return fail(positionals_[positional].metavar + " " + *error);
                }
                ++positional;
            }
        }
        if (positional < positionals_.size()) {
            return fail("expected " + positionals_[positional].metavar);
        }
        // A number of frames is a smoke test: headless, unless asked otherwise. Without a window, it's
        // one frame unless asked otherwise.
        if (spec_.window) {
            launch.window = !launch.frames || forceWindow;
            if (launch.capture && launch.window) {
                return fail("--capture runs headless: give --frames N, without --window");
            }
        } else if (!launch.frames) {
            launch.frames = 1;
        }
        return launch;
    }

    std::string usage() const {
        std::ostringstream out;
        out << "usage: " << spec_.program;
        for (const auto& p : positionals_) {
            out << " " << p.metavar;
        }
        out << " [options]\n";
        if (!spec_.about.empty()) {
            out << spec_.about << "\n";
        }
        std::vector<std::pair<std::string, std::string>> lines;
        for (const auto& p : positionals_) {
            lines.emplace_back(p.metavar, p.help);
        }
        for (const auto& o : options_) {
            lines.emplace_back(o.name + (o.metavar.empty() ? "" : " " + o.metavar), o.help);
        }
        if (spec_.window) {
            lines.emplace_back("--size WxH", "the window's size, or headless the image's (default " + std::to_string(spec_.window->width)
                                                 + "x" + std::to_string(spec_.window->height) + ")");
            lines.emplace_back("--window", "with --frames, run in a window");
        }
        lines.emplace_back("--frames N", spec_.window ? "run N frames, then exit (headless, unless --window)"
                                                      : "run N frames (default 1)");
        if (spec_.captureOption) {
            lines.emplace_back("--capture FILE", "write the last frame to a PNG file (with --frames)");
        }
        lines.emplace_back("--validation", "enable Vulkan validation; any message fails the run");
        lines.emplace_back("--plan", "print the render graph's plan");
        lines.emplace_back("--timings", "print each node's GPU time at the end (timestamps; each frame is waited for)");
        size_t width = 0;
        for (const auto& [left, _] : lines) {
            width = std::max(width, left.size());
        }
        for (const auto& [left, right] : lines) {
            out << "  " << left << std::string(width - left.size() + 2, ' ') << right << "\n";
        }
        return out.str();
    }

    const MainSpec& spec() const { return spec_; }

private:
    struct Option {
        std::string name;
        std::string metavar;
        std::string help;
        Setter set;
        bool takesValue;
    };

    const Option* find(const std::string& name) const {
        for (const auto& o : options_) {
            if (o.name == name) {
                return &o;
            }
        }
        return nullptr;
    }

    MainSpec spec_;
    std::vector<Option> options_;
    std::vector<Option> positionals_;
};

// Validation messages, from whichever thread calls Vulkan, printed and counted: any fails the run
class ValidationCounter {
public:
    std::function<void(VkDebugUtilsMessageSeverityFlagBitsEXT, const char*)> handler() {
        return [this](VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message) {
            ++count_;
            const char* label = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning";
            std::cerr << "[Vulkan validation " << label << "] " << message << std::endl;
        };
    }
    uint32_t count() const { return count_; }

private:
    std::atomic<uint32_t> count_ = 0;
};

// An image's pixels, RGBA8 tightly packed, as a capture copies them
struct Captured {
    std::vector<uint8_t> pixels;
    VkExtent2D extent;
};

// The most recent frame's copy of a readback node (an RGBA8 image's)
inline Result<Captured> captureReadback(RenderGraph& graph, NodeId node) {
    auto& readback = graph.nodeAs<ReadbackNode>(node);
    const VkFormat format = readback.format();
    if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB) {
        return Error(ErrorCode::Unsupported, "Captures are of RGBA8 images (rgba8 or rgba8-srgb)");
    }
    TRY_ASSIGN(auto pixels, readback.pixels());
    return Captured{std::move(pixels), readback.extent()};
}

inline Result<void> writePng(const std::filesystem::path& path, const Captured& captured) {
    const int width = static_cast<int>(captured.extent.width);
    if (!stbi_write_png(path.string().c_str(), width, static_cast<int>(captured.extent.height), 4, captured.pixels.data(),
                        width * 4)) {
        return Error(ErrorCode::NotFound, "Failed to write " + path.string());
    }
    return {};
}

// The end of a program: its error, validation messages and capture decide its exit status
inline int finish(const MainSpec& spec, const Launch& launch, const Result<void>& result, const ValidationCounter& validation,
                  const std::optional<Captured>& captured) {
    if (!result) {
        std::cerr << spec.program << ": " << result.error() << std::endl;
        return EXIT_FAILURE;
    }
    // Counted after the context is gone, so messages about leaks at destruction count too
    if (validation.count() > 0) {
        std::cerr << spec.program << ": " << validation.count() << " validation message(s)" << std::endl;
        return EXIT_FAILURE;
    }
    if (captured && launch.capture) {
        if (auto written = writePng(*launch.capture, *captured); !written) {
            std::cerr << spec.program << ": " << written.error() << std::endl;
            return EXIT_FAILURE;
        }
        if (spec.captureOption) {
            std::cout << "Wrote " << launch.capture->string() << std::endl;
        }
    }
    return EXIT_SUCCESS;
}

/*
 Each node's GPU times over a run (--timings): the graphs time their nodes (RenderGraph::
 setTimingEnabled), and each frame's are added, waiting for the frame. Printed by node, in the
 order nodes were first seen: the median, the fastest and the slowest frame's time.
 */
class TimingTable {
public:
    Result<void> add(RenderGraph& graph) {
        TRY_ASSIGN(const auto timings, graph.timings());
        for (const auto& timing : timings) {
            auto row = std::find_if(rows_.begin(), rows_.end(), [&](const Row& r) { return r.name == timing.name; });
            if (row == rows_.end()) {
                rows_.push_back({timing.name, {}});
                row = rows_.end() - 1;
            }
            row->milliseconds.push_back(timing.milliseconds);
        }
        return {};
    }

    void print(std::ostream& out) const {
        size_t width = 4;
        for (const auto& row : rows_) {
            width = std::max(width, row.name.size());
        }
        out << "GPU time per node, in milliseconds:\n"
            << "node" << std::string(width - 4, ' ') << "     median        min        max  frames\n";
        for (const auto& row : rows_) {
            auto sorted = row.milliseconds;
            std::sort(sorted.begin(), sorted.end());
            char line[128];
            std::snprintf(line, sizeof(line), "  %9.4f  %9.4f  %9.4f  %6zu", sorted[sorted.size() / 2], sorted.front(), sorted.back(),
                          sorted.size());
            out << row.name << std::string(width - row.name.size(), ' ') << line << "\n";
        }
        out << std::flush;
    }

private:
    struct Row {
        std::string name;
        std::vector<double> milliseconds;
    };
    std::vector<Row> rows_;
};

// Prints the active graph's plan, for --plan
inline Result<void> printPlan(RenderGraph& graph) {
    TRY_ASSIGN(const std::string plan, graph.dumpPlan());
    std::cout << plan << std::endl;
    return {};
}

/*
 Runs a program without a window (no window system): a context and graphs of its own, the app's
 frames (--frames, default 1), then the capture. create makes the app,
     Result<std::unique_ptr<App>>(VulkanContext&, const Launch&, const std::function<RenderGraph&()>& newGraph),
 and capture reads its last frame, Result<Captured>(App&).
 */
template<typename App, typename Create, typename Capture>
int runHeadless(const CommandLine& commandLine, const Launch& launch, Create&& create, Capture&& capture) {
    const MainSpec& spec = commandLine.spec();
    ValidationCounter validation;
    std::optional<Captured> captured;
    const Result<void> result = [&]() -> Result<void> {
        VulkanContextConfig config;
        config.applicationName = spec.program;
        config.enableValidation = launch.validation;
        config.enableSynchronizationValidation = launch.validation;
        config.onValidationMessage = validation.handler();
        config.features = App::requiredFeatures();
        TRY_ASSIGN(auto context, VulkanContext::create(std::move(config)));
        if (launch.validation && !context->validationEnabled()) {
            return Error(ErrorCode::Unsupported, "--validation was given, but the validation layer is unavailable");
        }
        // Destroyed before the context, after the app
        std::list<RenderGraph> graphs;
        TimingTable timings;
        return [&]() -> Result<void> {
            TRY_ASSIGN(auto app, create(*context, launch, std::function<RenderGraph&()>([&]() -> RenderGraph& {
                RenderGraph& graph = graphs.emplace_back(*context);
                graph.setTimingEnabled(launch.timings);
                return graph;
            })));
            for (uint32_t frame = 0; frame < *launch.frames && !app->quitRequested(); ++frame) {
                TRY(app->beginFrame());
                if (const auto size = app->takeResize()) {
                    app->setSurfaceExtent(*size);
                }
                TRY(app->renderGraph().execute());
                if (launch.plan && frame == 0) {
                    TRY(printPlan(app->renderGraph()));
                }
                if (launch.timings) {
                    TRY(timings.add(app->renderGraph()));
                }
            }
            VK_TRY(context->waitIdleAll(), "Failed to wait for the device");
            if (launch.timings) {
                timings.print(std::cout);
            }
            if (launch.capture) {
                TRY_ASSIGN(auto pixels, capture(*app));
                captured = std::move(pixels);
            }
            return {};
        }();
    }();
    return finish(spec, launch, result, validation, captured);
}

} // namespace zgal
