#include "core/VkTypes.h"
#include "core/QueueFamilyIndices.h"
#include "core/SwapChainSupportDetails.h"

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

    // The window is only queried when the surface leaves the extent up to the application
    const auto extent = details.chooseSwapExtent(nullptr);
    CHECK(extent.width == 640);
    CHECK(extent.height == 480);
}
