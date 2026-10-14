#include "VkTestContext.h"
#include "Image.h"

namespace {

std::vector<uint8_t> makePattern(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(width * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t* pixel = &pixels[(y * width + x) * 4];
            pixel[0] = static_cast<uint8_t>(x * 255 / (width - 1));
            pixel[1] = static_cast<uint8_t>(y * 255 / (height - 1));
            pixel[2] = static_cast<uint8_t>((x + y) % 256);
            pixel[3] = 255;
        }
    }
    return pixels;
}

} // namespace

TEST_CASE_FIXTURE(GpuTest, "Image::createFromUcharBuffer uploads pixels that read back unchanged") {
    auto& ctx = context();
    constexpr uint32_t width = 37;  // Deliberately not a multiple of 4 / 32
    constexpr uint32_t height = 19;
    auto pixels = makePattern(width, height);

    Handle<Image> image;
    Image::createFromUcharBuffer(image, ctx.resources(), pixels.data(), width, height, pixels.size(),
                                 ctx.queue(), ctx.commandPool(), ctx.device(), ctx.physicalDevice());
    REQUIRE(image);
    CHECK(image->getWidth() == width);
    CHECK(image->getHeight() == height);
    CHECK(image->getFormat() == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(image->getImageView() != VK_NULL_HANDLE);

    auto readback = image->readback(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    ctx.queue(), ctx.commandPool(), ctx.physicalDevice());
    CHECK(readback == pixels);

    // readback restores the layout, so reading twice must give the same result
    CHECK(image->readback(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          ctx.queue(), ctx.commandPool(), ctx.physicalDevice()) == pixels);
}

TEST_CASE_FIXTURE(GpuTest, "Image::createEmpty creates an image of the requested format and size") {
    auto& ctx = context();

    Handle<Image> image;
    Image::createEmpty(image, ctx.resources(), VK_FORMAT_R8G8B8A8_UNORM, 64, 32,
                       ctx.queue(), ctx.commandPool(), ctx.device(), ctx.physicalDevice());
    REQUIRE(image);
    CHECK(image->getFormat() == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(image->getWidth() == 64);
    CHECK(image->getHeight() == 32);

    auto readback = image->readback(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    ctx.queue(), ctx.commandPool(), ctx.physicalDevice());
    CHECK(readback.size() == 64 * 32 * 4);
}

TEST_CASE("Image::bytesPerPixel") {
    CHECK(Image::bytesPerPixel(VK_FORMAT_R8G8B8A8_UNORM) == 4);
    CHECK(Image::bytesPerPixel(VK_FORMAT_B8G8R8A8_SRGB) == 4);
    CHECK(Image::bytesPerPixel(VK_FORMAT_R32G32B32A32_SFLOAT) == 16);
    CHECK_THROWS_AS(Image::bytesPerPixel(VK_FORMAT_R8_UNORM), std::invalid_argument);
}
