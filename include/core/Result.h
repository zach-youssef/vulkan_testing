#pragma once

#include "core/Contract.h"

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

/*
 Runtime errors as values.

 Operations that can fail for reasons outside the program's control return a Result<T>: Vulkan
 failures (including device loss and running out of memory), missing or invalid files and shaders,
 devices lacking what's needed, and render graphs that don't compile. Programming errors are
 contract violations instead (see Contract.h).

 Result<T> holds a T or an Error and is shaped like C++23's std::expected<T, Error> (has_value,
 value, error, operator*, and_then, transform), so it can be replaced by it later. Unlike
 std::expected, an Error converts to a Result implicitly, so functions can `return error;`.
 value() on an error is a contract violation.

 Propagate errors with the macros:

    Result<Handle<Image>> loadTextures(VulkanContext& context) {
        TRY_ASSIGN(auto image, Image::createFromFile(context, path, ColorSpace::Srgb));    // declares image
        TRY(image->transitionLayout(...));                                // Result<void>, or any value discarded
        VK_TRY(vkSomething(...), "Failed to do something");             // a VkResult
        return image;
    }

 Errors carry a context chain, outermost first when described: add to it as an error passes
 through, e.g. TRY(loadTextures(context).withContext("loading the scene")).
 */

enum class ErrorCode {
    Vulkan,             // A Vulkan call failed; see Error::vkResult
    DeviceLost,         // VK_ERROR_DEVICE_LOST: the device can no longer be used
    OutOfMemory,        // Host or device memory ran out
    NoSuitableDevice,   // No device has what the context needs
    Unsupported,        // The device doesn't support something requested (e.g. a format's usage)
    NotFound,           // A file or shader couldn't be found or opened
    InvalidData,        // A file's contents couldn't be used (e.g. not SPIR-V, an undecodable image)
    InvalidGraph,       // A render graph doesn't compile
    Window,             // The window system failed
};

inline const char* errorCodeName(ErrorCode code) {
    switch (code) {
        case ErrorCode::Vulkan: return "Vulkan";
        case ErrorCode::DeviceLost: return "DeviceLost";
        case ErrorCode::OutOfMemory: return "OutOfMemory";
        case ErrorCode::NoSuitableDevice: return "NoSuitableDevice";
        case ErrorCode::Unsupported: return "Unsupported";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::InvalidData: return "InvalidData";
        case ErrorCode::InvalidGraph: return "InvalidGraph";
        case ErrorCode::Window: return "Window";
    }
    return "?";
}

struct Error {
    ErrorCode code;
    std::string message;
    // The failed Vulkan call's result, or VK_SUCCESS if the error didn't come from Vulkan
    VkResult vkResult = VK_SUCCESS;
    // What was being done when the error happened, innermost first (see withContext)
    std::vector<std::string> context;

    Error(ErrorCode code, std::string message, VkResult vkResult = VK_SUCCESS)
    : code(code), message(std::move(message)), vkResult(vkResult) {}

    // A failed Vulkan call. Device loss and running out of memory get their own codes.
    static Error vulkan(VkResult result, std::string message) {
        ErrorCode code = ErrorCode::Vulkan;
        if (result == VK_ERROR_DEVICE_LOST) {
            code = ErrorCode::DeviceLost;
        } else if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY
                   || result == VK_ERROR_OUT_OF_POOL_MEMORY) {
            code = ErrorCode::OutOfMemory;
        }
        return Error(code, std::move(message), result);
    }

    // Adds what was being done when the error happened (e.g. "loading the scene")
    Error& withContext(std::string what) & {
        context.push_back(std::move(what));
        return *this;
    }

    Error&& withContext(std::string what) && {
        context.push_back(std::move(what));
        return std::move(*this);
    }

    // The context, outermost first, then the message, e.g.
    // "loading the scene: loading 'brick.png': can't open file (VK_...)"
    std::string describe() const {
        std::string text;
        for (auto it = context.rbegin(); it != context.rend(); ++it) {
            text += *it + ": ";
        }
        text += message;
        if (vkResult != VK_SUCCESS) {
            text += std::string(" (") + string_VkResult(vkResult) + ")";
        }
        return text;
    }
};

inline std::ostream& operator<<(std::ostream& stream, const Error& error) {
    return stream << error.describe();
}

template<typename T>
class Result;

namespace detail {

template<typename T>
struct IsResult : std::false_type {};

template<typename T>
struct IsResult<Result<T>> : std::true_type {};

[[noreturn]] inline void badResultAccess(const Error& error) {
    contractViolation("value() of a Result holding an error: " + error.describe());
}

} // namespace detail

// Defined before the primary template, whose members use it
template<>
class [[nodiscard]] Result<void> {
public:
    using value_type = void;
    using error_type = Error;

    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    bool has_value() const { return !error_.has_value(); }
    explicit operator bool() const { return has_value(); }

    void value() const {
        if (error_) {
            detail::badResultAccess(*error_);
        }
    }

    void operator*() const { value(); }

    const Error& error() const& {
        if (!error_) {
            contractViolation("error() of a Result holding a value");
        }
        return *error_;
    }

    Error&& error() && {
        if (!error_) {
            contractViolation("error() of a Result holding a value");
        }
        return std::move(*error_);
    }

    // f() -> Result<U>, called on success; errors pass through
    template<typename F>
    auto and_then(F&& f) && {
        using R = std::invoke_result_t<F>;
        static_assert(detail::IsResult<R>::value, "and_then needs a function returning a Result");
        if (error_) {
            return R(std::move(*error_));
        }
        return std::forward<F>(f)();
    }

    template<typename F>
    auto transform(F&& f) && {
        using U = std::invoke_result_t<F>;
        if (error_) {
            return Result<U>(std::move(*error_));
        }
        if constexpr (std::is_void_v<U>) {
            std::forward<F>(f)();
            return Result<void>();
        } else {
            return Result<U>(std::forward<F>(f)());
        }
    }

    Result withContext(std::string what) && {
        if (error_) {
            error_->withContext(std::move(what));
        }
        return std::move(*this);
    }

private:
    std::optional<Error> error_;
};

template<typename T>
class [[nodiscard]] Result {
public:
    using value_type = T;
    using error_type = Error;

    Result() requires std::is_default_constructible_v<T> : storage_(std::in_place_index<0>) {}

    // A value, from anything T can be constructed from (e.g. Handle<Derived> for Handle<Base>)
    template<typename U = T>
        requires(std::is_constructible_v<T, U&&> && !std::is_same_v<std::remove_cvref_t<U>, Error>
                 && !detail::IsResult<std::remove_cvref_t<U>>::value)
    Result(U&& value) : storage_(std::in_place_index<0>, std::forward<U>(value)) {}

    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    // From a Result of a convertible type (e.g. Result<Handle<Derived>>)
    template<typename U>
        requires(!std::is_same_v<U, T> && !std::is_void_v<U> && std::is_constructible_v<T, U&&>)
    Result(Result<U>&& other)
    : storage_(other ? Storage(std::in_place_index<0>, std::move(*other))
                     : Storage(std::in_place_index<1>, std::move(other).error())) {}

    bool has_value() const { return storage_.index() == 0; }
    explicit operator bool() const { return has_value(); }

    T& value() & {
        check();
        return std::get<0>(storage_);
    }

    const T& value() const& {
        check();
        return std::get<0>(storage_);
    }

    T&& value() && {
        check();
        return std::get<0>(std::move(storage_));
    }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    template<typename U>
    T value_or(U&& fallback) const& {
        return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
    }

    // The error. Only valid when !has_value().
    const Error& error() const& {
        if (has_value()) {
            contractViolation("error() of a Result holding a value");
        }
        return std::get<1>(storage_);
    }

    Error&& error() && {
        if (has_value()) {
            contractViolation("error() of a Result holding a value");
        }
        return std::get<1>(std::move(storage_));
    }

    // f(T) -> Result<U>, called with the value; errors pass through
    template<typename F>
    auto and_then(F&& f) && {
        using R = std::invoke_result_t<F, T&&>;
        static_assert(detail::IsResult<R>::value, "and_then needs a function returning a Result");
        if (!has_value()) {
            return R(std::move(*this).error());
        }
        return std::forward<F>(f)(std::move(*this).value());
    }

    // f(T) -> U, giving a Result<U>; errors pass through
    template<typename F>
    auto transform(F&& f) && {
        using U = std::invoke_result_t<F, T&&>;
        if (!has_value()) {
            return Result<U>(std::move(*this).error());
        }
        if constexpr (std::is_void_v<U>) {
            std::forward<F>(f)(std::move(*this).value());
            return Result<void>();
        } else {
            return Result<U>(std::forward<F>(f)(std::move(*this).value()));
        }
    }

    // Adds context to an error (see Error::withContext); values pass through
    Result withContext(std::string what) && {
        if (!has_value()) {
            std::get<1>(storage_).withContext(std::move(what));
        }
        return std::move(*this);
    }

private:
    void check() const {
        if (!has_value()) {
            detail::badResultAccess(std::get<1>(storage_));
        }
    }

    using Storage = std::variant<T, Error>;
    Storage storage_;
};

// Checks a VkResult: VK_SUCCESS, or an Error describing what failed
inline Result<void> vkCheck(VkResult result, std::string what) {
    if (result != VK_SUCCESS) {
        return Error::vulkan(result, std::move(what));
    }
    return {};
}

// For main(): prints a failed result's error and returns EXIT_FAILURE, or returns EXIT_SUCCESS
inline int exitStatus(const Result<void>& result) {
    if (!result) {
        std::cerr << "Error: " << result.error() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#define VKW_CONCAT_INNER(a, b) a##b
#define VKW_CONCAT(a, b) VKW_CONCAT_INNER(a, b)
#define VKW_UNIQUE(name) VKW_CONCAT(name, __COUNTER__)

// Evaluates a Result and returns its error from the enclosing function if it holds one. The value,
// if any, is discarded.
#define TRY(expr) TRY_IMPL(VKW_UNIQUE(tryResult_), expr)
#define TRY_IMPL(result, expr)                    \
    do {                                          \
        auto result = (expr);                     \
        if (!result) {                            \
            return std::move(result).error();     \
        }                                         \
    } while (false)

// Evaluates a Result, returning its error from the enclosing function if it holds one, and
// otherwise initializes decl with its value, e.g. TRY_ASSIGN(auto image, Image::create(...)).
// decl is declared in the enclosing scope.
#define TRY_ASSIGN(decl, expr) TRY_ASSIGN_IMPL(VKW_UNIQUE(tryResult_), decl, expr)
#define TRY_ASSIGN_IMPL(result, decl, expr)       \
    auto result = (expr);                         \
    if (!result) {                                \
        return std::move(result).error();         \
    }                                             \
    decl = std::move(result).value()

// Evaluates a VkResult and returns an Error (Error::vulkan) with message from the enclosing
// function unless it is VK_SUCCESS
#define VK_TRY(expr, message) VK_TRY_IMPL(VKW_UNIQUE(vkResult_), expr, message)
#define VK_TRY_IMPL(result, expr, message)        \
    do {                                          \
        const VkResult result = (expr);           \
        if (result != VK_SUCCESS) {               \
            return Error::vulkan(result, message);\
        }                                         \
    } while (false)
