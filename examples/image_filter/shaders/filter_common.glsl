// Shared by the image filter's passes (image_filter.zgal). Every pass runs one invocation per pixel
// of the working image, which holds linear colour (see image_filter.cpp).
#ifndef FILTER_COMMON_GLSL
#define FILTER_COMMON_GLSL

// The dispatch is rounded up to whole workgroups, so some invocations fall outside the image
bool outside(ivec2 texel, ivec2 size) {
    return any(greaterThanEqual(texel, size));
}

// Reads a storage image (an accessor, e.g. source()), clamping to its edge so kernels don't darken
// the border
#define loadClamped(image, texel) imageLoad(image, clamp(texel, ivec2(0), imageSize(image) - 1))

// Rec. 709 luminance of linear colour
float luminance(vec3 linear) {
    return dot(linear, vec3(0.2126, 0.7152, 0.0722));
}

// The sRGB transfer functions (IEC 61966-2-1)
vec3 srgbEncode(vec3 linear) {
    linear = clamp(linear, 0.0, 1.0);
    return mix(linear * 12.92, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, greaterThan(linear, vec3(0.0031308)));
}

vec3 srgbDecode(vec3 encoded) {
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), greaterThan(encoded, vec3(0.04045)));
}

// Brightness as people perceive it (and as image editors show it): sRGB-encoded luminance
float perceivedBrightness(vec3 linear) {
    return srgbEncode(vec3(luminance(linear))).r;
}

#endif
