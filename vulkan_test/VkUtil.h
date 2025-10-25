#pragma once

#include <vulkan/vulkan.h>

#include <functional>

#define VK_SUCCESS_OR_THROW(resultExpr, errStr) \
if (resultExpr != VK_SUCCESS) { \
throw std::runtime_error(errStr);\
}

const std::vector<const char*> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

template<typename T>
std::vector<T> readVkVector(std::function<VkResult(uint32_t*, T*)> f) {
    uint32_t size = 0;
    VK_SUCCESS_OR_THROW(f(&size, nullptr), "Failed to read enumeration");
    std::vector<T> values{size};
    VK_SUCCESS_OR_THROW(f(&size, values.data()), "Failed to read enumeration");
    return values;
}

template<typename T, typename U>
std::vector<T> readVkVector(std::function<VkResult(U, uint32_t*, T*)> f) {
    uint32_t size = 0;
    VK_SUCCESS_OR_THROW(f(nullptr, &size, nullptr), "Failed to read enumeration");
    std::vector<T> values{size};
    VK_SUCCESS_OR_THROW(f(nullptr, &size, values.data()), "Failed to read enumeration");
    return values;
}

template<typename T, typename U>
std::vector<T> readVkVector(U u, std::function<VkResult(U, uint32_t*, T*)> f) {
    uint32_t size = 0;
    VK_SUCCESS_OR_THROW(f(u, &size, nullptr), "Failed to read enumeration");
    std::vector<T> values{size};
    VK_SUCCESS_OR_THROW(f(u, &size, values.data()), "Failed to read enumeration");
    return values;
}

template<typename T, typename U, typename V>
std::vector<T> readVkVector(U u, std::function<VkResult(U, V, uint32_t*, T*)> f) {
    uint32_t size = 0;
    VK_SUCCESS_OR_THROW(f(u, nullptr, &size, nullptr), "Failed to read enumeration");
    std::vector<T> values{size};
    VK_SUCCESS_OR_THROW(f(u, nullptr, &size, values.data()), "Failed to read enumeration");
    return values;
}

template<typename T, typename U, typename V>
std::vector<T> readVkVector(U u, V v, std::function<VkResult(U, V, uint32_t*, T*)> f) {
    uint32_t size = 0;
    VK_SUCCESS_OR_THROW(f(u, v, &size, nullptr), "Failed to read enumeration");
    std::vector<T> values{size};
    VK_SUCCESS_OR_THROW(f(u, v, &size, values.data()), "Failed to read enumeration");
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
