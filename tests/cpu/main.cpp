// Entry point for the CPU-only unit tests (no Vulkan device required)
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// Header-only library implementations, compiled exactly once per test binary
#define VK_WRAP_UTIL_IMPL
#include "VkTypes.h"
#include "DeviceSelection.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
