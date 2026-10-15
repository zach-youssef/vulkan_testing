// Compares two images for the image filter's tests:
//     image_diff <expected> <actual> <tolerance> [max differing fraction]
// Succeeds if they have the same size and no channel differs by more than tolerance (0-255), or,
// with a fraction, if at most that fraction of pixels do. GPUs and drivers may round differently,
// so outputs are compared with a tolerance rather than exactly.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "usage: image_diff <expected> <actual> <tolerance> [max differing fraction]" << std::endl;
        return 2;
    }
    const int tolerance = std::atoi(argv[3]);
    const double maxFraction = argc == 5 ? std::atof(argv[4]) : 0.0;

    int widths[2], heights[2], channels;
    stbi_uc* images[2];
    for (int i = 0; i < 2; ++i) {
        images[i] = stbi_load(argv[1 + i], &widths[i], &heights[i], &channels, STBI_rgb_alpha);
        if (!images[i]) {
            std::cerr << "image_diff: can't read '" << argv[1 + i] << "': " << stbi_failure_reason() << std::endl;
            return 1;
        }
    }
    if (widths[0] != widths[1] || heights[0] != heights[1]) {
        std::cerr << "image_diff: sizes differ: " << widths[0] << "x" << heights[0] << " and " << widths[1] << "x"
                  << heights[1] << std::endl;
        return 1;
    }

    const size_t pixels = static_cast<size_t>(widths[0]) * heights[0];
    size_t differing = 0;
    int maxDifference = 0;
    for (size_t p = 0; p < pixels; ++p) {
        int pixelDifference = 0;
        for (int c = 0; c < 4; ++c) {
            pixelDifference = std::max(pixelDifference, std::abs(images[0][p * 4 + c] - images[1][p * 4 + c]));
        }
        maxDifference = std::max(maxDifference, pixelDifference);
        differing += pixelDifference > tolerance;
    }
    stbi_image_free(images[0]);
    stbi_image_free(images[1]);

    const double fraction = static_cast<double>(differing) / pixels;
    std::cout << argv[2] << ": max difference " << maxDifference << ", " << differing << " pixel(s) beyond "
              << tolerance << std::endl;
    return fraction <= maxFraction ? 0 : 1;
}
