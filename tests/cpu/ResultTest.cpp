#include "core/Result.h"
#include "common/ResultChecks.h"

#include <doctest/doctest.h>

#include <memory>

namespace {

struct Base {
    virtual ~Base() = default;
};

struct Derived : Base {};

Result<int> parse(int value) {
    if (value < 0) {
        return Error(ErrorCode::InvalidData, "negative");
    }
    return value;
}

Result<int> doubled(int value) {
    TRY_ASSIGN(const int parsed, parse(value));
    return parsed * 2;
}

Result<void> check(int value) {
    TRY(parse(value).withContext("checking " + std::to_string(value)));
    return {};
}

Result<void> vulkanCall(VkResult result) {
    VK_TRY(result, "vkSomething failed");
    return {};
}

} // namespace

TEST_CASE("Result holds a value or an error") {
    Result<int> value = 3;
    CHECK(value);
    CHECK(value.has_value());
    CHECK(*value == 3);
    CHECK(value.value() == 3);
    CHECK(value.value_or(7) == 3);

    Result<int> error = Error(ErrorCode::NotFound, "missing");
    CHECK_FALSE(error);
    CHECK(error.error().code == ErrorCode::NotFound);
    CHECK(error.error().message == "missing");
    CHECK(error.value_or(7) == 7);

    Result<void> ok;
    CHECK(ok);
    Result<void> failed = Error(ErrorCode::Window, "no window");
    CHECK_FALSE(failed);
}

TEST_CASE("Using the value of a Result holding an error is a contract violation") {
    Result<int> error = Error(ErrorCode::NotFound, "missing");
    CHECK_THROWS_WITH_AS(error.value(), doctest::Contains("missing"), ContractViolation);
    CHECK_THROWS_AS(*error, ContractViolation);
    CHECK_THROWS_AS(Result<void>(Error(ErrorCode::NotFound, "missing")).value(), ContractViolation);
    // And the error of one holding a value
    CHECK_THROWS_AS(Result<int>(1).error(), ContractViolation);
    CHECK_THROWS_AS(Result<void>().error(), ContractViolation);
}

TEST_CASE("Results hold move-only values and convert between compatible types") {
    Result<std::unique_ptr<int>> owned = std::make_unique<int>(5);
    std::unique_ptr<int> taken = std::move(owned).value();
    CHECK(*taken == 5);

    Result<std::unique_ptr<Derived>> derived = std::make_unique<Derived>();
    Result<std::unique_ptr<Base>> base = std::move(derived);
    CHECK(base);
    CHECK(*base != nullptr);

    Result<std::unique_ptr<Derived>> failed = Error(ErrorCode::OutOfMemory, "full");
    Result<std::unique_ptr<Base>> failedBase = std::move(failed);
    CHECK(failedBase.error().code == ErrorCode::OutOfMemory);
}

TEST_CASE("TRY and TRY_ASSIGN propagate errors") {
    CHECK(doubled(4).value() == 8);
    CHECK_ERROR(doubled(-1), ErrorCode::InvalidData, "negative");

    CHECK_OK(check(1));
    CHECK_ERROR(check(-2), ErrorCode::InvalidData, "checking -2: negative");
}

TEST_CASE("VK_TRY turns failed VkResults into errors") {
    CHECK_OK(vulkanCall(VK_SUCCESS));
    CHECK_ERROR(vulkanCall(VK_ERROR_INITIALIZATION_FAILED), ErrorCode::Vulkan, "vkSomething failed (VK_ERROR_INITIALIZATION_FAILED)");
    CHECK_ERROR(vulkanCall(VK_ERROR_DEVICE_LOST), ErrorCode::DeviceLost, "");
    CHECK_ERROR(vulkanCall(VK_ERROR_OUT_OF_DEVICE_MEMORY), ErrorCode::OutOfMemory, "");
    CHECK(vulkanCall(VK_ERROR_DEVICE_LOST).error().vkResult == VK_ERROR_DEVICE_LOST);
}

TEST_CASE("Errors describe their context outermost first") {
    Error error(ErrorCode::NotFound, "can't open brick.png");
    error.withContext("loading textures").withContext("loading the scene");
    CHECK(error.describe() == "loading the scene: loading textures: can't open brick.png");

    auto result = Result<int>(Error(ErrorCode::NotFound, "gone")).withContext("inner").withContext("outer");
    CHECK(result.error().describe() == "outer: inner: gone");
    // Values pass through withContext untouched
    CHECK(Result<int>(2).withContext("unused").value() == 2);
}

TEST_CASE("and_then and transform chain on values and pass errors through") {
    CHECK(parse(3).and_then(doubled).value() == 6);
    CHECK_ERROR(parse(-3).and_then(doubled), ErrorCode::InvalidData, "negative");
    CHECK(parse(3).transform([](int value) { return value + 1; }).value() == 4);
    CHECK_FALSE(parse(-3).transform([](int value) { return value + 1; }));

    bool ran = false;
    CHECK(Result<void>().and_then([] { return parse(1); }).value() == 1);
    CHECK_OK(Result<void>().transform([&ran] { ran = true; }));
    CHECK(ran);
}

TEST_CASE("exitStatus reports failures") {
    CHECK(exitStatus(Result<void>()) == EXIT_SUCCESS);
    CHECK(exitStatus(Result<void>(Error(ErrorCode::Window, "no display"))) == EXIT_FAILURE);
}
