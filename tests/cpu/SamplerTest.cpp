#include "core/VkTypes.h"

#include <doctest/doctest.h>

TEST_CASE("Samplers filter anisotropically only when they filter linearly") {
    CHECK(SamplerDesc{}.anisotropic());
    CHECK_FALSE(SamplerDesc{.anisotropy = false}.anisotropic());
    // A nearest sampler picks texels, whatever its anisotropy
    CHECK_FALSE(SamplerDesc{.filter = VK_FILTER_NEAREST}.anisotropic());
    CHECK_FALSE(SamplerDesc{.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST}.anisotropic());
}
