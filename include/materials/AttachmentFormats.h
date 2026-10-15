#pragma once

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

// The attachment formats a graphics pipeline renders to. With dynamic rendering, pipelines are
// built against these (VkPipelineRenderingCreateInfo) instead of a render pass, and can draw into
// any render target with the same formats.
struct AttachmentFormats {
    std::vector<VkFormat> color;
    VkFormat depth = VK_FORMAT_UNDEFINED;

    bool operator==(const AttachmentFormats&) const = default;

    std::string describe() const {
        std::string text = "color [";
        for (size_t i = 0; i < color.size(); ++i) {
            text += (i ? ", " : "") + std::to_string(color[i]);
        }
        return text + "], depth " + std::to_string(depth);
    }
};
