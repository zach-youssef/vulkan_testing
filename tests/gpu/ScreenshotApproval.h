#pragma once

#include <cstdint>
#include <string>
#include <vector>

/*
 Screenshot ("golden image") testing built on ApprovalTests.cpp and NVIDIA FLIP.

 verifyScreenshot() writes the rendered image to
     tests/gpu/approved/<device>/<TestFile>.<TestName>.received.png
 and compares it against the matching .approved.png using the FLIP perceptual metric,
 rather than exact pixel equality, so small rasterization / filtering differences
 between driver versions do not cause failures.

 Approved images are stored per device (screenshotDirectoryName), since different GPUs
 legitimately render slightly differently. On the first run on a device (or after an intentional rendering
 change) the test fails and leaves the .received.png behind; inspect it and run
 `make approve-screenshots` to accept it. When images differ, a FLIP error heatmap is
 written next to the received image as <...>.flip.png.
 */

struct ScreenshotTolerance {
    // Maximum mean FLIP error over the image (0 = identical, 1 = maximally different)
    float maxMeanError = 0.05f;
    // Pixels with a FLIP error above this are considered visibly different...
    float highErrorThreshold = 0.5f;
    // ...and at most this fraction of pixels may be visibly different. This catches
    // small localized defects (e.g. a missing triangle) that barely move the mean.
    float maxHighErrorFraction = 0.001f;
};

// Verifies tightly packed 8-bit RGBA pixels (sRGB encoded) against the approved screenshot
void verifyScreenshot(const std::vector<uint8_t>& rgbaPixels,
                      uint32_t width,
                      uint32_t height,
                      const ScreenshotTolerance& tolerance = {});

// The directory under approved/ holding a device's screenshots: its name without a final
// " (LLVM ...)" suffix (which names a software rasterizer's build, so an LLVM update doesn't move
// lavapipe's from "llvmpipe"; other suffixes, which name the chip, stay), with characters other
// than letters, digits, '-' and '.' replaced by '_'
std::string screenshotDirectoryName(const std::string& deviceName);

// Results of a FLIP comparison, exposed for testing the comparison itself
struct FlipResult {
    float meanError;
    float highErrorFraction;
};

// Compares two equally sized, tightly packed 8-bit RGBA (sRGB) images with FLIP. If
// errorMapPath is non-empty, writes a magma-colored FLIP error heatmap there.
FlipResult compareWithFlip(const std::vector<uint8_t>& referenceRgba,
                           const std::vector<uint8_t>& testRgba,
                           uint32_t width,
                           uint32_t height,
                           float highErrorThreshold,
                           const std::string& errorMapPath = "");
