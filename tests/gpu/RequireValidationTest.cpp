#include "zgal/Main.h"
#include "common/ResultChecks.h"
#include "VkTestContext.h"

#include <doctest/doctest.h>

namespace {

std::unique_ptr<VulkanContext> contextWith(bool validation, bool synchronizationValidation) {
    VulkanContextConfig config;
    config.applicationName = "requireValidation tests";
    config.enableValidation = validation;
    config.enableSynchronizationValidation = synchronizationValidation;
    return VulkanContext::create(std::move(config)).value();
}

} // namespace

TEST_CASE("zgal::requireValidation fails --validation runs lacking validation or synchronization validation") {
    zgal::Launch validated;
    validated.validation = true;
    const zgal::Launch unvalidated;

    const auto none = contextWith(false, false);
    CHECK_ERROR(zgal::requireValidation(validated, *none), ErrorCode::Unsupported, "validation layer");
    // Without --validation nothing is required
    CHECK(zgal::requireValidation(unvalidated, *none).has_value());

    if (ValidationOptOuts::fromEnvironment().validation) {
        MESSAGE("Validation disabled by the environment; skipping the validation-only context");
        return;
    }
    const auto validationOnly = contextWith(true, false);
    requireValidation(*validationOnly, false);
    CHECK_ERROR(zgal::requireValidation(validated, *validationOnly), ErrorCode::Unsupported, "synchronization validation");
}
