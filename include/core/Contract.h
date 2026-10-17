#pragma once

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

/*
 Contract violations: programming errors, as opposed to runtime errors (see Result.h).

 Misusing the wrapper (drawing without an active graph, asking a headless app for its window,
 indexing an unknown node, declaring a write with read(), ...) is a bug in the calling code, not
 something an application can handle. Such misuse calls contractViolation(), which runs the
 installed handler. The default handler prints the message and aborts.

 A handler that returns is followed by an abort, so a handler either ends the program or throws.
 Tests install throwingContractViolationHandler so that misuse can be checked with
 CHECK_THROWS_AS(..., ContractViolation).
 */

// Thrown by throwingContractViolationHandler
class ContractViolation : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

using ContractViolationHandler = void (*)(const std::string& message);

inline void abortingContractViolationHandler(const std::string& message) {
    std::cerr << "Contract violation: " << message << std::endl;
    std::abort();
}

inline void throwingContractViolationHandler(const std::string& message) {
    throw ContractViolation(message);
}

namespace detail {

inline std::atomic<ContractViolationHandler>& contractViolationHandler() {
    static std::atomic<ContractViolationHandler> handler{abortingContractViolationHandler};
    return handler;
}

} // namespace detail

// Installs the handler called on contract violations and returns the previous one. Thread safe.
inline ContractViolationHandler setContractViolationHandler(ContractViolationHandler handler) {
    return detail::contractViolationHandler().exchange(handler ? handler : abortingContractViolationHandler);
}

[[noreturn]] inline void contractViolation(const std::string& message) {
    detail::contractViolationHandler().load()(message);
    // Handlers must not return
    std::abort();
}
