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
 The validation GPU tests may run without: VK_TEST_DISABLE_VALIDATION=1 runs them without
 validation, VK_TEST_DISABLE_SYNC_VALIDATION=1 without synchronization validation only. Otherwise
 a context lacking either fails the tests (requireValidation), rather than passing unchecked.
 */
struct ValidationOptOuts {
    bool validation = false;
    bool synchronization = false;

    // From the variables' values (null when unset); only "1" opts out
    static ValidationOptOuts parse(const char* disableValidation, const char* disableSyncValidation) {
        const auto set = [](const char* value) { return value && strcmp(value, "1") == 0; };
        return {set(disableValidation), set(disableSyncValidation)};
    }

    static ValidationOptOuts fromEnvironment() {
        return parse(std::getenv("VK_TEST_DISABLE_VALIDATION"), std::getenv("VK_TEST_DISABLE_SYNC_VALIDATION"));
    }
};

// What a context that has validation and synchronization validation as given lacks of what a test
// asked for (synchronization validation too, with syncRequested), minus the opt-outs; empty if
// nothing
inline std::string missingValidation(bool validation, bool synchronizationValidation, bool syncRequested,
                                     const ValidationOptOuts& optOuts) {
    if (optOuts.validation) {
        return {};
    }
    if (!validation) {
        return "the validation layer (VK_LAYER_KHRONOS_validation) is unavailable";
    }
    if (syncRequested && !optOuts.synchronization && !synchronizationValidation) {
        return "synchronization validation (VK_EXT_validation_features) is unavailable";
    }
    return {};
}

// Fails the test unless the context has the validation it asked for (or the environment opts out)
inline void requireValidation(const VulkanContext& context, bool syncRequested) {
    const std::string missing = missingValidation(context.validationEnabled(), context.synchronizationValidationEnabled(),
                                                  syncRequested, ValidationOptOuts::fromEnvironment());
    // Synchronization validation is only missing when validation is there
    const std::string optOut = context.validationEnabled() ? "VK_TEST_DISABLE_SYNC_VALIDATION" : "VK_TEST_DISABLE_VALIDATION";
    const std::string message = missing + "; set " + optOut + "=1 to run without it";
    REQUIRE_MESSAGE(missing.empty(), message);
}

/*
 Headless VulkanContext shared by all GPU tests.

 The Khronos validation layer is enabled, with synchronization validation, unless
 ValidationOptOuts says otherwise, and every validation error is recorded so that tests can fail
 on API misuse and synchronization hazards (see GpuTest below), which also fails every test when
 the context lacks either.
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
        const ValidationOptOuts optOuts = ValidationOptOuts::fromEnvironment();
        VulkanContextConfig config;
        config.applicationName = "vulkan_test GPU tests";
        config.assetSearchPaths = {TEST_ASSET_DIR};   // Models and images (tests/gpu/assets)
        // Indirect draws of several commands, whose firstInstance chooses a material (IndirectTest)
        config.features.core.features.multiDrawIndirect = VK_TRUE;
        config.features.core.features.drawIndirectFirstInstance = VK_TRUE;
        config.enableValidation = !optOuts.validation;
        config.enableSynchronizationValidation = !optOuts.synchronization;
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
 doctest fixture for GPU tests. Fails the test if the context lacks validation or synchronization
 validation (unless ValidationOptOuts says so), if it produced validation errors, left
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
        requireValidation(context(), true);
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
