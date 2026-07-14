#pragma once

#include "debug/DebugLoc.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace pred {

struct ErrorContext {
    std::string stage;
    std::string source_file;
    DebugLoc loc;
    std::string note;

    bool empty() const {
        return stage.empty() && source_file.empty() && !loc.valid() &&
               note.empty();
    }
};

class RTLZZException : public std::runtime_error {
public:
    explicit RTLZZException(std::string message);
    RTLZZException(ErrorContext context, std::string message);
    RTLZZException(std::vector<ErrorContext> context_stack, std::string message);

    const std::string& message() const noexcept { return message_; }
    const std::vector<ErrorContext>& contextStack() const noexcept {
        return context_stack_;
    }
    std::optional<ErrorContext> primaryContext() const;

private:
    std::string message_;
    std::vector<ErrorContext> context_stack_;
};

class ErrorContextGuard {
public:
    explicit ErrorContextGuard(ErrorContext context);
    ErrorContextGuard(std::string stage,
                      DebugLoc loc = {},
                      std::string note = {});
    ErrorContextGuard(std::string stage,
                      std::string source_file,
                      DebugLoc loc,
                      std::string note = {});
    ~ErrorContextGuard();

    ErrorContextGuard(const ErrorContextGuard&) = delete;
    ErrorContextGuard& operator=(const ErrorContextGuard&) = delete;
    ErrorContextGuard(ErrorContextGuard&& other) noexcept;
    ErrorContextGuard& operator=(ErrorContextGuard&& other) noexcept = delete;

private:
    bool active_ = false;
};

// Compatibility alias for the common misspelling in planning notes.
using ErrorContexGuard = ErrorContextGuard;

const std::vector<ErrorContext>& currentErrorContextStack();
std::optional<ErrorContext> currentErrorContext();

std::string formatErrorContext(const ErrorContext& context);
std::string formatRTLZZExceptionMessage(
    const std::string& message,
    const std::vector<ErrorContext>& context_stack);

[[noreturn]] void throwRTLZZ(std::string message);
[[noreturn]] void throwRTLZZ(ErrorContext context, std::string message);

} // namespace pred
