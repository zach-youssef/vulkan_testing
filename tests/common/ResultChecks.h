#pragma once

#include "core/Result.h"

#include <doctest/doctest.h>

#include <string>

/*
 doctest checks for Results. Getting the value of a Result holding an error (value(), operator*)
 is a contract violation, which the tests' handler turns into a ContractViolation exception, so a
 test can simply use .value(): an error fails the test with its description.
 */

// Checks that expr (a Result) holds an error with errorCode whose description contains text
#define CHECK_ERROR(expr, errorCode, text)                                                        \
    do {                                                                                          \
        const auto checkErrorResult_ = (expr);                                                    \
        REQUIRE_MESSAGE(!checkErrorResult_.has_value(), "expected an error from " #expr);         \
        INFO("error: " << checkErrorResult_.error().describe());                                  \
        CHECK(checkErrorResult_.error().code == (errorCode));                                     \
        CHECK(checkErrorResult_.error().describe().find(text) != std::string::npos);              \
    } while (false)

// Checks that expr (a Result) holds a value, reporting the error otherwise
#define CHECK_OK(expr)                                                                            \
    do {                                                                                          \
        const auto checkOkResult_ = (expr);                                                       \
        CHECK_MESSAGE(checkOkResult_.has_value(),                                                 \
                      (checkOkResult_.has_value() ? std::string() : checkOkResult_.error().describe())); \
    } while (false)
