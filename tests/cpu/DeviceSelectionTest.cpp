#include "core/VkTypes.h"
#include "core/QueueFamilyIndices.h"
#include "core/SwapChainSupportDetails.h"
#include "core/DeviceFeatures.h"

#include <doctest/doctest.h>

#include <algorithm>

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
    CHECK(missing.size() == 17);
    CHECK(std::find(missing.begin(), missing.end(), "shaderSampledImageArrayNonUniformIndexing") != missing.end());
    CHECK(missing.front() == "samplerAnisotropy");
    CHECK(std::find(missing.begin(), missing.end(), "descriptorBindingUpdateUnusedWhilePending") != missing.end());
    CHECK(missing.back() == "dynamicRendering");

    CHECK(DeviceFeatures::required().missingRequired().empty());

    auto partial = DeviceFeatures::required();
    partial.vulkan13.dynamicRendering = VK_FALSE;
    CHECK(partial.missingRequired() == std::vector<std::string>{"dynamicRendering"});
}

TEST_CASE("Optional features are enabled where a device supports them") {
    DeviceFeatures supported = DeviceFeatures::required();
    CHECK(DeviceFeatures::optional().common(supported).missing(DeviceFeatures::optional())
          == std::vector<std::string>{"drawIndirectCount"});
    supported.vulkan12.drawIndirectCount = VK_TRUE;
    supported.vulkan12.shaderInt8 = VK_TRUE;
    const DeviceFeatures enabled = DeviceFeatures::optional().common(supported);
    CHECK(enabled.missing(DeviceFeatures::optional()).empty());
    // Only the optional ones
    CHECK(enabled.vulkan12.shaderInt8 == VK_FALSE);
    CHECK(enabled.core.features.samplerAnisotropy == VK_FALSE);
}

TEST_CASE("DeviceFeatures keeps its pNext chain intact when copied") {
    auto features = DeviceFeatures::required();
    features.vulkan11.storageBuffer16BitAccess = VK_TRUE;
    DeviceFeatures copy = features;
    CHECK(copy.chain() == &copy.core);
    CHECK(copy.core.pNext == &copy.vulkan11);
    CHECK(copy.vulkan11.pNext == &copy.vulkan12);
    CHECK(copy.vulkan12.pNext == &copy.vulkan13);
    CHECK(copy.vulkan13.pNext == nullptr);
    CHECK(copy.vulkan11.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    CHECK(copy.vulkan11.storageBuffer16BitAccess == VK_TRUE);
    CHECK(copy.vulkan13.dynamicRendering == VK_TRUE);
}

TEST_CASE("Extensions' features join the chain, with their extension, only when enabled") {
    DeviceFeatures features = DeviceFeatures::required();
    CHECK(features.extensions().empty());
    features.chain();
    CHECK(features.vulkan13.pNext == nullptr);

    features.atomicFloat.shaderBufferFloat32AtomicAdd = VK_TRUE;
    REQUIRE(features.extensions().size() == 1);
    CHECK(std::string(features.extensions()[0]) == "VK_EXT_shader_atomic_float");
    features.chain();
    CHECK(features.vulkan13.pNext == &features.atomicFloat);
    CHECK(features.atomicFloat.sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT);
    CHECK(features.atomicFloat.pNext == nullptr);
    // Copies keep it
    DeviceFeatures copy = features;
    CHECK(copy.vulkan13.pNext == &copy.atomicFloat);
    // And a device without it lacks it by name
    CHECK(DeviceFeatures::required().missing(features) == std::vector<std::string>{"shaderBufferFloat32AtomicAdd"});
}

TEST_CASE("DeviceFeatures names every feature of every structure") {
    // The first and last feature of each structure map to their names
    DeviceFeatures none;
    DeviceFeatures needed;
    needed.core.features.robustBufferAccess = VK_TRUE;
    needed.core.features.inheritedQueries = VK_TRUE;
    needed.vulkan11.storageBuffer16BitAccess = VK_TRUE;
    needed.vulkan11.shaderDrawParameters = VK_TRUE;
    needed.vulkan12.samplerMirrorClampToEdge = VK_TRUE;
    needed.vulkan12.subgroupBroadcastDynamicId = VK_TRUE;
    needed.vulkan13.robustImageAccess = VK_TRUE;
    needed.vulkan13.maintenance4 = VK_TRUE;
    needed.atomicFloat.shaderBufferFloat32Atomics = VK_TRUE;
    needed.atomicFloat.sparseImageFloat32AtomicAdd = VK_TRUE;
    CHECK(none.missing(needed) == std::vector<std::string>{
        "robustBufferAccess", "inheritedQueries",
        "storageBuffer16BitAccess", "shaderDrawParameters",
        "samplerMirrorClampToEdge", "subgroupBroadcastDynamicId",
        "robustImageAccess", "maintenance4",
        "shaderBufferFloat32Atomics", "sparseImageFloat32AtomicAdd",
    });
    CHECK(needed.missing(needed).empty());
    CHECK(needed.missing(none).empty());
}

TEST_CASE("DeviceFeatures combines the wrapper's required features with an application's") {
    DeviceFeatures extra;
    extra.core.features.shaderInt16 = VK_TRUE;
    extra.vulkan11.storageBuffer16BitAccess = VK_TRUE;
    extra.vulkan12.storageBuffer8BitAccess = VK_TRUE;
    extra.vulkan12.shaderInt8 = VK_TRUE;

    auto combined = DeviceFeatures::requiredWith(extra);
    CHECK(combined.missingRequired().empty());
    CHECK(combined.missing(extra).empty());
    // Adding never disables a feature
    combined.add(DeviceFeatures());
    CHECK(combined.vulkan13.dynamicRendering == VK_TRUE);
    CHECK(combined.vulkan12.shaderInt8 == VK_TRUE);

    // A device with only the wrapper's features lacks the application's
    CHECK(DeviceFeatures::required().missing(combined) == std::vector<std::string>{
        "shaderInt16", "storageBuffer16BitAccess", "storageBuffer8BitAccess", "shaderInt8",
    });
}
