#include "VkTestContext.h"

TEST_CASE_FIXTURE(GpuTest, "GPU test context") {
    auto& ctx = context();
    MESSAGE("Running GPU tests on: " << ctx.deviceName());

    CHECK(ctx.device() != VK_NULL_HANDLE);
    CHECK(ctx.queue(QueueType::Graphics) != VK_NULL_HANDLE);
    // The fixture requires validation and synchronization validation unless the environment opts out
    const ValidationOptOuts optOuts = ValidationOptOuts::fromEnvironment();
    CHECK(ctx.validationEnabled() == !optOuts.validation);
    CHECK(ctx.synchronizationValidationEnabled() == !(optOuts.validation || optOuts.synchronization));
}
