// Entry point for the GPU tests (requires a Vulkan device, but no window)

// ApprovalTests provides doctest's main along with its doctest integration
#define APPROVALS_DOCTEST
#include "ApprovalTests.hpp"

// Header-only library implementations, compiled exactly once per test binary
#define VK_WRAP_UTIL_IMPL
#include "VkTypes.h"
#include "CommandUtil.h"
#include "DeviceSelection.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
