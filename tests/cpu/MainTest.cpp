#include <doctest/doctest.h>

#include "zgal/Main.h"

#include <string>
#include <vector>

namespace {

// Parses args (after the program's name) for a headless program with a --capture option
std::variant<zgal::Launch, int> parse(std::vector<std::string> args) {
    zgal::CommandLine commandLine(zgal::MainSpec{.program = "program", .captureOption = true});
    std::vector<char*> argv{const_cast<char*>("program")};
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return commandLine.parse(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST_CASE("Generated mains capture several frames into a directory, running as many frames as they need") {
    const auto several = parse({"--capture-frames", "8,2,120,2", "--capture", "shots"});
    REQUIRE(std::holds_alternative<zgal::Launch>(several));
    const auto& launch = std::get<zgal::Launch>(several);
    CHECK(launch.captureFrames == std::vector<uint32_t>{2, 8, 120});
    CHECK(launch.frames == 120u);
    CHECK(launch.capture == std::filesystem::absolute("shots"));
    CHECK(launch.mode().capture());

    const auto more = parse({"--capture-frames", "1", "--capture", "shots", "--frames", "5"});
    REQUIRE(std::holds_alternative<zgal::Launch>(more));
    CHECK(std::get<zgal::Launch>(more).frames == 5u);

    // A bad command line exits with status 2
    CHECK(std::get<int>(parse({"--capture-frames", "1,2"})) == 2);                              // No directory
    CHECK(std::get<int>(parse({"--capture-frames", "1,x", "--capture", "d"})) == 2);
    CHECK(std::get<int>(parse({"--capture-frames", "0", "--capture", "d"})) == 2);              // Frames count from 1
    CHECK(std::get<int>(parse({"--capture-frames", "9", "--capture", "d", "--frames", "4"})) == 2);
}
