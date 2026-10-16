#pragma once

#include "core/Result.h"

#include <vulkan/vulkan.h>

#include <functional>
#include <vector>

const std::vector<const char*> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

// Reads a Vulkan enumeration (count, then values). Fails only if the enumeration does.

template<typename T>
Result<std::vector<T>> readVkVector(std::function<VkResult(uint32_t*, T*)> f) {
    uint32_t size = 0;
    VK_TRY(f(&size, nullptr), "Failed to read enumeration");
    std::vector<T> values{size};
    VK_TRY(f(&size, values.data()), "Failed to read enumeration");
    return values;
}

template<typename T, typename U>
Result<std::vector<T>> readVkVector(std::function<VkResult(U, uint32_t*, T*)> f) {
    uint32_t size = 0;
    VK_TRY(f(nullptr, &size, nullptr), "Failed to read enumeration");
    std::vector<T> values{size};
    VK_TRY(f(nullptr, &size, values.data()), "Failed to read enumeration");
    return values;
}

template<typename T, typename U>
Result<std::vector<T>> readVkVector(U u, std::function<VkResult(U, uint32_t*, T*)> f) {
    uint32_t size = 0;
    VK_TRY(f(u, &size, nullptr), "Failed to read enumeration");
    std::vector<T> values{size};
    VK_TRY(f(u, &size, values.data()), "Failed to read enumeration");
    return values;
}

template<typename T, typename U, typename V>
Result<std::vector<T>> readVkVector(U u, std::function<VkResult(U, V, uint32_t*, T*)> f) {
    uint32_t size = 0;
    VK_TRY(f(u, nullptr, &size, nullptr), "Failed to read enumeration");
    std::vector<T> values{size};
    VK_TRY(f(u, nullptr, &size, values.data()), "Failed to read enumeration");
    return values;
}

template<typename T, typename U, typename V>
Result<std::vector<T>> readVkVector(U u, V v, std::function<VkResult(U, V, uint32_t*, T*)> f) {
    uint32_t size = 0;
    VK_TRY(f(u, v, &size, nullptr), "Failed to read enumeration");
    std::vector<T> values{size};
    VK_TRY(f(u, v, &size, values.data()), "Failed to read enumeration");
    return values;
}

template<typename T, typename U>
std::vector<T> readVkVectorVoid(U u, std::function<void(U, uint32_t*, T*)> f) {
    uint32_t size = 0;
    f(u, &size, nullptr);
    std::vector<T> values{size};
    f(u, &size, values.data());
    return values;
}
