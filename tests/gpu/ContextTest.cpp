#include "VkTestContext.h"

TEST_CASE_FIXTURE(GpuTest, "GPU test context") {
    auto& ctx = context();
    MESSAGE("Running GPU tests on: " << ctx.deviceName());

    CHECK(ctx.device() != VK_NULL_HANDLE);
    CHECK(ctx.queue(QueueType::Graphics) != VK_NULL_HANDLE);
    // Not a hard failure: validation layers may be unavailable on some machines
    WARN_MESSAGE(ctx.validationEnabled(), "Validation layers unavailable; validation errors will not be caught");
}
