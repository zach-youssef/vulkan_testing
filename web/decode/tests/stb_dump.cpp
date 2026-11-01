// Decodes an image file as native code does (stbi_load(..., 4), stb_image as include/resources/Image.h
// uses it) and writes what it gives, for checking the web's decoder (web/decode) against it:
//     stb_dump <image> <out>
// out gets the width and height (32-bit little-endian) and then the RGBA8 pixels. A file that can't
// be decoded exits with 1, printing stb_image's reason alone on stderr; a bad command line with 2
// and a failed write with 3.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cstdint>
#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: stb_dump <image> <out>" << std::endl;
        return 2;
    }
    int width, height, channels;
    stbi_uc* pixels = stbi_load(argv[1], &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        std::cerr << stbi_failure_reason() << std::endl;
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    for (uint32_t value : {uint32_t(width), uint32_t(height)}) {
        const char bytes[4] = {char(value), char(value >> 8), char(value >> 16), char(value >> 24)};
        out.write(bytes, 4);
    }
    out.write(reinterpret_cast<const char*>(pixels), std::streamsize(width) * height * 4);
    stbi_image_free(pixels);
    if (!out) {
        std::cerr << "stb_dump: can't write '" << argv[2] << "'" << std::endl;
        return 3;
    }
    return 0;
}
