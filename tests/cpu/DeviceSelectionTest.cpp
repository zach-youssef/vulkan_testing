#include "core/VkTypes.h"
#include "core/QueueFamilyIndices.h"
#include "core/SwapChainSupportDetails.h"
#include "core/DeviceFeatures.h"

#include <doctest/doctest.h>

TEST_CASE("QueueFamilyIndices requires both a graphics and a present family") {
    QueueFamilyIndices indices;
    CHECK_FALSE(indices.complete());
    CHECK(indices.uniqueQueueFamilies().empty());

    indices.graphicsFamily = 0;
    CHECK_FALSE(indices.complete());
    CHECK(indices.uniqueQueueFamilies().empty());

    indices.presentFamily = 1;
    CHECK(indices.complete());
}

TEST_CASE("QueueFamilyIndices::uniqueQueueFamilies de-duplicates shared families") {
    QueueFamilyIndices indices;
    indices.graphicsFamily = 2;

    SUBCASE("same family") {
        indices.presentFamily = 2;
        CHECK(indices.uniqueQueueFamilies() == std::set<uint32_t>{2});
    }
    SUBCASE("different families") {
        indices.presentFamily = 0;
        CHECK(indices.uniqueQueueFamilies() == std::set<uint32_t>{0, 2});
    }
}

TEST_CASE("SwapChainSupportDetails::adequate requires formats and present modes") {
    SwapChainSupportDetails details{};
    CHECK_FALSE(details.adequate());

    details.formats.push_back({VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
    CHECK_FALSE(details.adequate());

    details.presentModes.push_back(VK_PRESENT_MODE_FIFO_KHR);
    CHECK(details.adequate());
}

TEST_CASE("SwapChainSupportDetails::chooseSwapSurfaceFormat falls back to the first format") {
    SwapChainSupportDetails details{};
    details.formats = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };

    CHECK(details.chooseSwapSurfaceFormat().format == VK_FORMAT_R8G8B8A8_UNORM);
}

TEST_CASE("SwapChainSupportDetails::chooseSwapSurfaceFormat prefers B8G8R8A8_SRGB with an sRGB color space") {
    SwapChainSupportDetails details{};
    details.formats = {
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT},
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };

    const auto chosen = details.chooseSwapSurfaceFormat();
    CHECK(chosen.format == VK_FORMAT_B8G8R8A8_SRGB);
    CHECK(chosen.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
}

TEST_CASE("SwapChainSupportDetails::chooseSwapPresentMode prefers mailbox, else FIFO") {
    SwapChainSupportDetails details{};

    SUBCASE("mailbox available") {
        details.presentModes = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR};
        CHECK(details.chooseSwapPresentMode() == VK_PRESENT_MODE_MAILBOX_KHR);
    }
    SUBCASE("mailbox unavailable") {
        details.presentModes = {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR};
        CHECK(details.chooseSwapPresentMode() == VK_PRESENT_MODE_FIFO_KHR);
    }
}

TEST_CASE("SwapChainSupportDetails::chooseSwapExtent uses the surface's current extent when defined") {
    SwapChainSupportDetails details{};
    details.capabilities.currentExtent = {640, 480};

    // The framebuffer size is only used when the surface leaves the extent up to the application
    const auto extent = details.chooseSwapExtent({1024, 768});
    CHECK(extent.width == 640);
    CHECK(extent.height == 480);
}

TEST_CASE("SwapChainSupportDetails::chooseSwapExtent clamps the framebuffer size when the surface leaves it undefined") {
    SwapChainSupportDetails details{};
    details.capabilities.currentExtent = {UINT32_MAX, UINT32_MAX};
    details.capabilities.minImageExtent = {100, 100};
    details.capabilities.maxImageExtent = {2000, 1000};

    CHECK(details.chooseSwapExtent({800, 600}).width == 800);
    CHECK(details.chooseSwapExtent({800, 600}).height == 600);
    CHECK(details.chooseSwapExtent({50, 4000}).width == 100);
    CHECK(details.chooseSwapExtent({50, 4000}).height == 1000);
}

TEST_CASE("DeviceFeatures reports the required features a device lacks") {
    DeviceFeatures none;
    auto missing = none.missingRequired();
    CHECK(missing == std::vector<std::string>{"samplerAnisotropy", "timelineSemaphore", "synchronization2", "dynamicRendering"});

    CHECK(DeviceFeatures::required().missingRequired().empty());

    auto partial = DeviceFeatures::required();
    partial.vulkan13.dynamicRendering = VK_FALSE;
    CHECK(partial.missingRequired() == std::vector<std::string>{"dynamicRendering"});
}

TEST_CASE("DeviceFeatures keeps its pNext chain intact when copied") {
    auto features = DeviceFeatures::required();
    DeviceFeatures copy = features;
    CHECK(copy.chain() == &copy.core);
    CHECK(copy.core.pNext == &copy.vulkan12);
    CHECK(copy.vulkan12.pNext == &copy.vulkan13);
    CHECK(copy.vulkan13.pNext == nullptr);
    CHECK(copy.vulkan13.dynamicRendering == VK_TRUE);
}
