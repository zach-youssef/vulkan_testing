#include "VkTestContext.h"
#include "resources/Image.h"
#include "TestShaders.h"

#include <fstream>
#include <iterator>

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

    auto image = Image::createFromUcharBuffer(ctx, pixels.data(), width, height, pixels.size(), ColorSpace::Srgb).value();
    REQUIRE(image);
    CHECK(image->getWidth() == width);
    CHECK(image->getHeight() == height);
    CHECK(image->getFormat() == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(image->getImageView() != VK_NULL_HANDLE);

    auto readback = image->readback(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).value();
    CHECK(readback == pixels);

    // readback restores the layout, so reading twice must give the same result
    CHECK(image->readback(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).value() == pixels);
}

TEST_CASE_FIXTURE(GpuTest, "Image::createEmpty creates an image of the requested format and size") {
    auto& ctx = context();

    auto image = Image::createEmpty(ctx, VK_FORMAT_R8G8B8A8_UNORM, 64, 32).value();
    REQUIRE(image);
    CHECK(image->getFormat() == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(image->getWidth() == 64);
    CHECK(image->getHeight() == 32);

    auto readback = image->readback(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).value();
    CHECK(readback.size() == 64 * 32 * 4);
}

TEST_CASE_FIXTURE(GpuTest, "Sampled images are not created with storage usage") {
    auto& ctx = context();
    std::vector<uint8_t> pixels(4 * 4 * 4, 255);

    // sRGB formats are rarely supported as storage images, so requesting storage usage for them
    // would fail on many devices
    auto uploaded = Image::createFromUcharBuffer(ctx, pixels.data(), 4, 4, pixels.size(), ColorSpace::Srgb).value();
    CHECK(uploaded->getFormat() == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK((uploaded->getUsage() & VK_IMAGE_USAGE_SAMPLED_BIT) != 0);
    CHECK((uploaded->getUsage() & VK_IMAGE_USAGE_STORAGE_BIT) == 0);

    auto empty = Image::createEmptyRGBA(ctx, 4, 4).value();
    CHECK(empty->getFormat() == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK((empty->getUsage() & VK_IMAGE_USAGE_SAMPLED_BIT) != 0);
    CHECK((empty->getUsage() & VK_IMAGE_USAGE_STORAGE_BIT) == 0);
}

TEST_CASE_FIXTURE(GpuTest, "Image::createStorage creates a storage image in the GENERAL layout") {
    auto& ctx = context();
    REQUIRE(Image::supportsUsage(ctx, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                                 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));

    auto image = Image::createStorage(ctx, VK_FORMAT_R16G16B16A16_SFLOAT, 16, 8).value();
    CHECK(image->getFormat() == VK_FORMAT_R16G16B16A16_SFLOAT);
    CHECK((image->getUsage() & VK_IMAGE_USAGE_STORAGE_BIT) != 0);
    // Read back from GENERAL, the layout createStorage leaves it in
    CHECK(image->readback(VK_IMAGE_LAYOUT_GENERAL).value().size() == 16 * 8 * 8);
}

TEST_CASE_FIXTURE(GpuTest, "Image::create rejects usages the format does not support") {
    auto& ctx = context();
    // Depth formats can never be storage images
    const VkFormat depth = VK_FORMAT_D32_SFLOAT;
    CHECK_FALSE(Image::supportsUsage(ctx, depth, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT));
    CHECK(Image::supportsUsage(ctx, depth, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));

    CHECK_ERROR(Image::createStorage(ctx, depth, 4, 4), ErrorCode::Unsupported, "does not support image usage");
}

TEST_CASE("Image::requiredFormatFeatures maps each usage to its format feature") {
    CHECK(Image::requiredFormatFeatures(0) == 0);
    CHECK(Image::requiredFormatFeatures(VK_IMAGE_USAGE_STORAGE_BIT) == VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    CHECK(Image::requiredFormatFeatures(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
          == (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT));
    CHECK(Image::requiredFormatFeatures(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
          == VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

TEST_CASE("Image::bytesPerPixel") {
    CHECK(Image::bytesPerPixel(VK_FORMAT_R8G8B8A8_UNORM) == 4);
    CHECK(Image::bytesPerPixel(VK_FORMAT_B8G8R8A8_SRGB) == 4);
    CHECK(Image::bytesPerPixel(VK_FORMAT_R32G32B32A32_SFLOAT) == 16);
    CHECK(Image::bytesPerPixel(VK_FORMAT_R8_UNORM) == 1);
    CHECK(Image::bytesPerPixel(VK_FORMAT_R16_SFLOAT) == 2);
    CHECK(Image::bytesPerPixel(VK_FORMAT_R32_UINT) == 4);
    CHECK(Image::bytesPerPixel(VK_FORMAT_R32G32_SFLOAT) == 8);
    CHECK_THROWS_AS(Image::bytesPerPixel(VK_FORMAT_R8G8B8_UNORM), ContractViolation);
}

TEST_CASE_FIXTURE(GpuTest, "Loading an image that can't be read names the file and the reason") {
    auto& ctx = context();
    CHECK_ERROR(Image::createFromFile(ctx, "missing/texture.png", ColorSpace::Srgb), ErrorCode::NotFound,
                "Image 'missing/texture.png' not found");

    // A file that isn't an image
    CHECK_ERROR(Image::createFromFile(ctx, testShaderDir() + "/pattern.comp.spv", ColorSpace::Srgb), ErrorCode::InvalidData,
                "pattern.comp.spv': ");
}

TEST_CASE_FIXTURE(GpuTest, "Images load from files or encoded bytes in the colour space given") {
    auto& ctx = context();
    const auto path = ctx.findAsset("quadrants.png").value();
    auto srgb = Image::createFromFile(ctx, path, ColorSpace::Srgb).value();
    auto linear = Image::createFromFile(ctx, path, ColorSpace::Linear).value();
    CHECK(srgb->getFormat() == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(linear->getFormat() == VK_FORMAT_R8G8B8A8_UNORM);
    CHECK(linear->getWidth() == 2);
    CHECK(linear->getHeight() == 2);

    // The same file's bytes, decoded from memory
    std::ifstream file(path, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    auto decoded = Image::createFromEncoded(ctx, bytes.data(), bytes.size(), ColorSpace::Linear, "the test's bytes").value();
    CHECK(decoded->getWidth() == 2);
    const std::vector<uint8_t> garbage{1, 2, 3};
    CHECK_ERROR(Image::createFromEncoded(ctx, garbage.data(), garbage.size(), ColorSpace::Linear, "the test's garbage"),
                ErrorCode::InvalidData, "Failed to decode the test's garbage: ");
}

TEST_CASE_FIXTURE(GpuTest, "Assets are found in the asset search paths") {
    auto& ctx = context();
    CHECK(ctx.findAsset("quadrants.png").value() == std::filesystem::path(TEST_ASSET_DIR) / "quadrants.png");
    // Absolute paths are used as they are
    const auto absolute = std::filesystem::path(TEST_ASSET_DIR) / "quadrants.png";
    CHECK(ctx.findAsset(absolute).value() == absolute);
    CHECK_ERROR(ctx.findAsset("missing.png"), ErrorCode::NotFound,
                std::string("Asset 'missing.png' not found in the asset search paths (") + TEST_ASSET_DIR + ")");
}

TEST_CASE_FIXTURE(GpuTest, "Images used only for transfers have no view") {
    auto& ctx = context();
    auto image = Image::create(ctx, 4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_IMAGE_ASPECT_COLOR_BIT).value();
    CHECK_THROWS_WITH_AS((void)image->getImageView(), doctest::Contains("no view"), ContractViolation);
}
