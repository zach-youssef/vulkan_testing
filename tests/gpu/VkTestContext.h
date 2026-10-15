#pragma once

#include "core/VulkanContext.h"

#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

/*
 Headless VulkanContext shared by all GPU tests.

 The Khronos validation layer is enabled when available, and every validation error is
 recorded so that tests can fail on API misuse (see GpuTest below). Set
 VK_TEST_DISABLE_VALIDATION=1 to run without it.
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
        config.enableValidation = !(disableValidation && strcmp(disableValidation, "1") == 0);
        config.onValidationMessage = [this](VkDebugUtilsMessageSeverityFlagBitsEXT severity, const char* message) {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
                std::lock_guard<std::mutex> lock(mutex_);
                validationErrors_.emplace_back(message);
            }
        };

        context_ = std::make_unique<VulkanContext>(std::move(config));
    }

    std::mutex mutex_;
    std::vector<std::string> validationErrors_;

    std::unique_ptr<VulkanContext> context_;
};

/*
 doctest fixture for GPU tests. Fails the test if it produced validation errors or
 left resources alive in the shared ResourceManager.

     TEST_CASE_FIXTURE(GpuTest, "...") { VulkanContext& ctx = context(); ... }
 */
class GpuTest {
public:
    GpuTest() : resourcesBefore_(context().resources().size()) {
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
        const size_t resourcesAfter = context().resources().size();
        if (resourcesAfter != resourcesBefore_) {
            FAIL_CHECK("Test leaked handles to managed resources: " << resourcesBefore_
                       << " before, " << resourcesAfter << " after");
        }
    }

protected:
    static VulkanContext& context() {
        return VkTestContext::get().vulkan();
    }

private:
    size_t resourcesBefore_;
};
