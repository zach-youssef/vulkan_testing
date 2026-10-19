#include "core/Platform.h"

#include <doctest/doctest.h>

TEST_CASE("executableDirectory is the directory holding the running test executable") {
    const auto directory = executableDirectory();
    REQUIRE_FALSE(directory.empty());
    CHECK(directory.is_absolute());
    CHECK(std::filesystem::exists(directory / "cpu_tests"));
}

TEST_CASE("Shaders and assets are found beside the executable by default") {
    CHECK(defaultShaderSearchPaths() == std::vector<std::string>{(executableDirectory() / "shaders").string()});
    CHECK(defaultAssetSearchPaths() == std::vector<std::string>{(executableDirectory() / "assets").string()});
}
