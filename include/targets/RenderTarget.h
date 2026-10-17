#pragma once

#include "graph/GraphTypes.h"

#include <array>
#include <optional>
#include <vector>

/*
 The attachments a graphics node renders to, as render graph images. A CLEAR or DONT_CARE load
 discards an attachment's previous contents.
 */

struct ColorAttachment {
    ImageId image;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE;
    std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 1.0f};
};

struct DepthAttachment {
    ImageId image;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    float clear = 1.0f;
};

struct RenderTarget {
    std::vector<ColorAttachment> color;
    std::optional<DepthAttachment> depth;
};
