#include "VkTestContext.h"
#include "OffscreenRenderTarget.h"

TEST_CASE_FIXTURE(GpuTest, "OffscreenRenderTarget clears and reads back its color image") {
    auto& ctx = context();
    constexpr uint32_t width = 33;
    constexpr uint32_t height = 17;

    OffscreenRenderTarget target(ctx, width, height, VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(target.getExtent().width == width);
    CHECK(target.getExtent().height == height);
    CHECK(target.getColorImage()->getFormat() == VK_FORMAT_R8G8B8A8_UNORM);

    ctx.submitImmediate([&target](VkCommandBuffer commandBuffer) {
        // 51 / 255 = 0.2 etc., so the cleared values are exact in UNORM
        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = {{51.0f / 255.0f, 102.0f / 255.0f, 153.0f / 255.0f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        beginInfo.renderPass = target.getRenderPass();
        beginInfo.framebuffer = target.getFramebuffer();
        beginInfo.renderArea = {{0, 0}, target.getExtent()};
        beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        beginInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(commandBuffer);
    });

    auto pixels = target.readback();
    REQUIRE(pixels.size() == width * height * 4);

    std::vector<uint8_t> expected;
    for (uint32_t i = 0; i < width * height; ++i) {
        expected.insert(expected.end(), {51, 102, 153, 255});
    }
    CHECK(pixels == expected);
}
