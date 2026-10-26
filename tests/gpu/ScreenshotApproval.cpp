#include "ScreenshotApproval.h"
#include "VkTestContext.h"

#include "ApprovalTests.hpp"
#include "FLIP.h"

#include <stb_image.h>
#include "util/StbImageWrite.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>

namespace {

// Writes the rendered pixels as the "received" PNG
class PngWriter : public ApprovalTests::ApprovalWriter {
public:
    PngWriter(const std::vector<uint8_t>& rgbaPixels, uint32_t width, uint32_t height)
    : pixels_(rgbaPixels), width_(width), height_(height) {}

    std::string getFileExtensionWithDot() const override {
        return ".png";
    }

    void write(std::string path) const override {
        if (!stbi_write_png(path.c_str(), static_cast<int>(width_), static_cast<int>(height_),
                            4, pixels_.data(), static_cast<int>(width_ * 4))) {
            throw std::runtime_error("Failed to write screenshot to " + path);
        }
    }

    void cleanUpReceived(std::string receivedPath) const override {
        std::remove(receivedPath.c_str());
    }

private:
    const std::vector<uint8_t>& pixels_;
    uint32_t width_;
    uint32_t height_;
};

struct LoadedImage {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
};

LoadedImage loadPng(const std::string& path) {
    LoadedImage image;
    int channels = 0;
    stbi_uc* data = stbi_load(path.c_str(), &image.width, &image.height, &channels, STBI_rgb_alpha);
    if (!data) {
        throw std::runtime_error("Failed to load " + path);
    }
    image.rgba.assign(data, data + image.width * image.height * 4);
    stbi_image_free(data);
    return image;
}

std::string errorMapPathFor(const std::string& receivedPath) {
    const std::string suffix = ".received.png";
    if (receivedPath.size() > suffix.size()
        && receivedPath.compare(receivedPath.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return receivedPath.substr(0, receivedPath.size() - suffix.size()) + ".flip.png";
    }
    return receivedPath + ".flip.png";
}

// Treats two PNGs as equivalent when they are perceptually close according to FLIP
class FlipComparator : public ApprovalTests::ApprovalComparator {
public:
    explicit FlipComparator(ScreenshotTolerance tolerance) : tolerance_(tolerance) {}

    void setTolerance(ScreenshotTolerance tolerance) {
        tolerance_ = tolerance;
    }

    bool contentsAreEquivalent(std::string receivedPath, std::string approvedPath) const override {
        const auto received = loadPng(receivedPath);
        const auto approved = loadPng(approvedPath);
        if (received.width != approved.width || received.height != approved.height) {
            MESSAGE("Screenshot size " << received.width << "x" << received.height
                    << " does not match approved size " << approved.width << "x" << approved.height);
            return false;
        }

        const auto errorMapPath = errorMapPathFor(receivedPath);
        const auto result = compareWithFlip(approved.rgba, received.rgba,
                                            static_cast<uint32_t>(received.width),
                                            static_cast<uint32_t>(received.height),
                                            tolerance_.highErrorThreshold, errorMapPath);

        const bool equivalent = result.meanError <= tolerance_.maxMeanError
                                && result.highErrorFraction <= tolerance_.maxHighErrorFraction;
        if (equivalent) {
            std::remove(errorMapPath.c_str());
        } else {
            MESSAGE("FLIP mean error " << result.meanError << " (max " << tolerance_.maxMeanError << "), "
                    << result.highErrorFraction * 100.0f << "% of pixels above " << tolerance_.highErrorThreshold
                    << " (max " << tolerance_.maxHighErrorFraction * 100.0f << "%). Error map: " << errorMapPath);
        }
        return equivalent;
    }

private:
    ScreenshotTolerance tolerance_;
};


FLIP::image<FLIP::color3> toLinearFlipImage(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    std::vector<float> rgb(static_cast<size_t>(width) * height * 3);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        for (int channel = 0; channel < 3; ++channel) {
            rgb[i * 3 + channel] = rgba[i * 4 + channel] / 255.0f;
        }
    }
    FLIP::image<FLIP::color3> image(static_cast<int>(width), static_cast<int>(height));
    image.setPixels(rgb.data(), static_cast<int>(width), static_cast<int>(height));
    // FLIP expects linear RGB input; screenshots are sRGB encoded
    image.sRGBToLinearRGB();
    return image;
}

} // namespace

std::string screenshotDirectoryName(const std::string& deviceName) {
    // An LLVM suffix names the software rasterizer's build, e.g. lavapipe's
    // "llvmpipe (LLVM 20.1.2, 256 bits)", so drop it. Other suffixes stay: Mesa's hardware drivers
    // name the chip there, e.g. "AMD Radeon Graphics (RADV REMBRANDT)"
    std::string name = deviceName;
    if (const auto open = name.rfind(" (LLVM "); open != std::string::npos && name.back() == ')') {
        name.erase(open);
    }
    while (!name.empty() && name.back() == ' ') {
        name.pop_back();
    }
    // Filesystem friendly, e.g. "Apple M4" -> "Apple_M4"
    for (char& c : name) {
        c = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '.' ? c : '_';
    }
    return name;
}

FlipResult compareWithFlip(const std::vector<uint8_t>& referenceRgba,
                           const std::vector<uint8_t>& testRgba,
                           uint32_t width,
                           uint32_t height,
                           float highErrorThreshold,
                           const std::string& errorMapPath) {
    if (referenceRgba.size() != testRgba.size() || referenceRgba.size() != static_cast<size_t>(width) * height * 4) {
        throw std::invalid_argument("compareWithFlip: image sizes do not match");
    }

    auto reference = toLinearFlipImage(referenceRgba, width, height);
    auto test = toLinearFlipImage(testRgba, width, height);

    FLIP::Parameters parameters; // Default viewing conditions
    FLIP::image<float> errorMap(static_cast<int>(width), static_cast<int>(height), 0.0f);
    FLIP::evaluate(reference, test, false /* useHDR */, parameters, errorMap);

    double sum = 0.0;
    size_t highErrorPixels = 0;
    for (int y = 0; y < static_cast<int>(height); ++y) {
        for (int x = 0; x < static_cast<int>(width); ++x) {
            const float error = errorMap.get(x, y);
            sum += error;
            highErrorPixels += error > highErrorThreshold ? 1 : 0;
        }
    }
    const double pixelCount = static_cast<double>(width) * height;

    if (!errorMapPath.empty()) {
        FLIP::image<FLIP::color3> heatmap(static_cast<int>(width), static_cast<int>(height));
        heatmap.colorMap(errorMap, FLIP::magmaMap);
        std::vector<uint8_t> heatmapRgb(static_cast<size_t>(width) * height * 3);
        for (int y = 0; y < static_cast<int>(height); ++y) {
            for (int x = 0; x < static_cast<int>(width); ++x) {
                const auto color = heatmap.get(x, y);
                uint8_t* pixel = &heatmapRgb[(static_cast<size_t>(y) * width + x) * 3];
                pixel[0] = static_cast<uint8_t>(std::clamp(color.x, 0.0f, 1.0f) * 255.0f + 0.5f);
                pixel[1] = static_cast<uint8_t>(std::clamp(color.y, 0.0f, 1.0f) * 255.0f + 0.5f);
                pixel[2] = static_cast<uint8_t>(std::clamp(color.z, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
        stbi_write_png(errorMapPath.c_str(), static_cast<int>(width), static_cast<int>(height),
                       3, heatmapRgb.data(), static_cast<int>(width * 3));
    }

    return FlipResult{static_cast<float>(sum / pixelCount),
                      static_cast<float>(highErrorPixels / pixelCount)};
}

void verifyScreenshot(const std::vector<uint8_t>& rgbaPixels,
                      uint32_t width,
                      uint32_t height,
                      const ScreenshotTolerance& tolerance) {
    REQUIRE(rgbaPixels.size() == static_cast<size_t>(width) * height * 4);

    // Registered once; ApprovalTests looks comparators up by file extension
    static auto comparator = std::make_shared<FlipComparator>(tolerance);
    static auto comparatorRegistration =
        ApprovalTests::FileApprover::registerComparatorForExtension(".png", comparator);
    comparator->setTolerance(tolerance);

    const std::string subdirectoryName = "approved/" + screenshotDirectoryName(VkTestContext::get().vulkan().deviceName());
    // ApprovalTests only creates a single missing directory level (with 0733 permissions), so
    // create the per-device directory ourselves. Screenshot tests live alongside this file.
    std::filesystem::create_directories(std::filesystem::path(__FILE__).parent_path() / subdirectoryName);
    auto subdirectory = ApprovalTests::Approvals::useApprovalsSubdirectory(subdirectoryName);

    // Do not launch external diff tools; failures report the file paths instead
    ApprovalTests::Approvals::verify(PngWriter(rgbaPixels, width, height),
                                     ApprovalTests::Options().withReporter(ApprovalTests::QuietReporter()));
}
