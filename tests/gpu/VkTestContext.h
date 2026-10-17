#pragma once

#include "core/VulkanContext.h"
#include "shaders/ShaderLibrary.h"
#include "common/ResultChecks.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

/*
 Headless VulkanContext shared by all GPU tests.

 The Khronos validation layer is enabled when available, with synchronization validation, and
 every validation error is recorded so that tests can fail on API misuse and synchronization
 hazards (see GpuTest below). Set VK_TEST_DISABLE_VALIDATION=1 to run without validation, or
 VK_TEST_DISABLE_SYNC_VALIDATION=1 to run without synchronization validation only.
 */
class VkTestContext {
public:
    // Created on first use. Destroyed by shutdown() at the end of main(), rather than during
    // static destruction, where the validation layer's own globals may already be gone.
    static VkTestContext& get() {
        auto& context = storage();
        if (!context) {
            context.reset(new VkTestContext());
        }
        return *context;
    }

    static void shutdown() {
        storage().reset();
    }

    VulkanContext& vulkan() { return *context_; }

    // Returns and clears the validation errors recorded so far
    std::vector<std::string> takeValidationErrors() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(validationErrors_, {});
    }


private:
    static std::unique_ptr<VkTestContext>& storage() {
        static std::unique_ptr<VkTestContext> context;
        return context;
    }

    VkTestContext() {
        const char* disableValidation = std::getenv("VK_TEST_DISABLE_VALIDATION");

        VulkanContextConfig config;
        config.applicationName = "vulkan_test GPU tests";
        config.shaderSearchPaths = {TEST_SHADER_DIR};
        config.assetSearchPaths = {TEST_ASSET_DIR};   // Models and images (tests/gpu/assets)
        const char* disableSyncValidation = std::getenv("VK_TEST_DISABLE_SYNC_VALIDATION");
        config.enableValidation = !(disableValidation && strcmp(disableValidation, "1") == 0);
        config.enableSynchronizationValidation = !(disableSyncValidation && strcmp(disableSyncValidation, "1") == 0);
        config.onValidationMessage = [this](VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message) {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                std::lock_guard<std::mutex> lock(mutex_);
                validationErrors_.emplace_back(message);
            }
        };

        context_ = VulkanContext::create(std::move(config)).value();
    }

    std::mutex mutex_;
    std::vector<std::string> validationErrors_;

    std::unique_ptr<VulkanContext> context_;
};

/*
 doctest fixture for GPU tests. Fails the test if it produced validation errors, left
 resources alive in the shared ResourceManager, left PerFrame collections alive or changed the
 number of frames in flight (tests that change it must restore it).

     TEST_CASE_FIXTURE(GpuTest, "...") { VulkanContext& ctx = context(); ... }
 */
class GpuTest {
public:
    GpuTest()
    : resourcesBefore_(managedResources()),
    collectionsBefore_(context().frames().size()),
    framesInFlightBefore_(context().framesInFlight()) {
        // Errors raised outside of a test (e.g. creating the context) are reported here
        for (const auto& error : VkTestContext::get().takeValidationErrors()) {
            FAIL_CHECK("Vulkan validation error before this test: " << error);
        }
    }

    ~GpuTest() {
        context().waitIdleAll();
        for (const auto& error : VkTestContext::get().takeValidationErrors()) {
            FAIL_CHECK("Vulkan validation error: " << error);
        }
        const size_t resourcesAfter = managedResources();
        if (resourcesAfter != resourcesBefore_) {
            FAIL_CHECK("Test leaked handles to managed resources: " << resourcesBefore_
                       << " before, " << resourcesAfter << " after");
        }
        const size_t collectionsAfter = context().frames().size();
        if (collectionsAfter != collectionsBefore_) {
            FAIL_CHECK("Test leaked PerFrame collections: " << collectionsBefore_
                       << " before, " << collectionsAfter << " after");
        }
        if (context().framesInFlight() != framesInFlightBefore_) {
            FAIL_CHECK("Test changed the number of frames in flight from " << framesInFlightBefore_
                       << " to " << context().framesInFlight());
        }
    }

protected:
    static VulkanContext& context() {
        return VkTestContext::get().vulkan();
    }

private:
    // Managed resources, other than the shaders the library caches for later tests
    static size_t managedResources() {
        return context().resources().size() - context().shaders().size();
    }
public:

private:
    size_t resourcesBefore_;
    size_t collectionsBefore_;
    uint32_t framesInFlightBefore_;
};
