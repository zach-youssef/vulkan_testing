// Entry point for the GPU tests (requires a Vulkan device, but no window)

// ApprovalTests provides doctest along with its doctest integration; main() is defined below
#define APPROVALS_DOCTEST_EXISTING_MAIN
#include "ApprovalTests.hpp"

// Header-only library implementations, compiled exactly once per test binary
#define VK_WRAP_UTIL_IMPL
#include "core/VkTypes.h"
#include "core/DeviceSelection.h"
#include "resources/Model.h"   // cgltf and tinyobjloader

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "util/StbImageWrite.h"

#include "VkTestContext.h"

#include "core/Contract.h"

int main(int argc, char** argv) {
    // Misuse throws ContractViolation, so tests can check it (CHECK_THROWS_AS)
    setContractViolationHandler(throwingContractViolationHandler);

    doctest::Context context(argc, argv);
    int result = context.run();

    // Tear down the shared Vulkan context before static destruction
    VkTestContext::shutdown();

    return result;
}
