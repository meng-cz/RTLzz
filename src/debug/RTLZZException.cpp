#include "debug/RTLZZException.h"

#include <sstream>
#include <utility>

namespace pred {
namespace {

thread_local std::vector<ErrorContext> error_context_stack;

std::vector<ErrorContext> stackWithContext(ErrorContext context) {
    auto stack = error_context_stack;
    if (!context.empty()) stack.push_back(std::move(context));
    return stack;
}

std::string locationText(const ErrorContext& context) {
    DebugLoc loc = context.loc;
    if (loc.file.empty() && !context.source_file.empty()) loc.file = context.source_file;
    std::ostringstream os;
    if (!loc.file.empty()) os << loc.file;
    if (loc.line > 0) {
        if (!loc.file.empty()) os << ":";
        os << loc.line;
        if (loc.column > 0) os << ":" << loc.column;
    } else if (loc.column > 0) {
        if (!loc.file.empty()) os << ":";
        os << "column " << loc.column;
    }
    if (loc.end_line > 0) {
        os << "-";
        if (loc.end_line != loc.line) os << loc.end_line;
        if (loc.end_column > 0) os << ":" << loc.end_column;
    }
    return os.str();
}

} // namespace

RTLZZException::RTLZZException(std::string message)
    : RTLZZException(error_context_stack, std::move(message)) {}

RTLZZException::RTLZZException(ErrorContext context, std::string message)
    : RTLZZException(stackWithContext(std::move(context)), std::move(message)) {}

RTLZZException::RTLZZException(std::vector<ErrorContext> context_stack,
                               std::string message)
    : std::runtime_error(formatRTLZZExceptionMessage(message, context_stack)),
      message_(std::move(message)),
      context_stack_(std::move(context_stack)) {}

std::optional<ErrorContext> RTLZZException::primaryContext() const {
    if (context_stack_.empty()) return std::nullopt;
    return context_stack_.back();
}

ErrorContextGuard::ErrorContextGuard(ErrorContext context) {
    error_context_stack.push_back(std::move(context));
    active_ = true;
}

ErrorContextGuard::ErrorContextGuard(std::string stage,
                                     DebugLoc loc,
                                     std::string note)
    : ErrorContextGuard(ErrorContext{std::move(stage),
                                     loc.file,
                                     std::move(loc),
                                     std::move(note)}) {}

ErrorContextGuard::ErrorContextGuard(std::string stage,
                                     std::string source_file,
                                     DebugLoc loc,
                                     std::string note)
    : ErrorContextGuard(ErrorContext{std::move(stage),
                                     std::move(source_file),
                                     std::move(loc),
                                     std::move(note)}) {}

ErrorContextGuard::~ErrorContextGuard() {
    if (active_ && !error_context_stack.empty()) {
        error_context_stack.pop_back();
    }
}

ErrorContextGuard::ErrorContextGuard(ErrorContextGuard&& other) noexcept
    : active_(other.active_) {
    other.active_ = false;
}

const std::vector<ErrorContext>& currentErrorContextStack() {
    return error_context_stack;
}

std::optional<ErrorContext> currentErrorContext() {
    if (error_context_stack.empty()) return std::nullopt;
    return error_context_stack.back();
}

std::string formatErrorContext(const ErrorContext& context) {
    std::ostringstream os;
    bool wrote = false;
    if (!context.stage.empty()) {
        os << "stage=" << context.stage;
        wrote = true;
    }
    auto loc = locationText(context);
    if (!loc.empty()) {
        if (wrote) os << " ";
        os << "at " << loc;
        wrote = true;
    }
    if (!context.note.empty()) {
        if (wrote) os << " ";
        os << "(" << context.note << ")";
    }
    return os.str();
}

std::string formatRTLZZExceptionMessage(
    const std::string& message,
    const std::vector<ErrorContext>& context_stack) {
    std::ostringstream os;
    os << message;
    if (context_stack.empty()) return os.str();

    const auto primary = formatErrorContext(context_stack.back());
    if (!primary.empty()) os << " [" << primary << "]";

    if (context_stack.size() > 1) {
        os << "\ncontext stack:";
        for (auto it = context_stack.rbegin(); it != context_stack.rend(); ++it) {
            auto text = formatErrorContext(*it);
            if (!text.empty()) os << "\n  - " << text;
        }
    }
    return os.str();
}

[[noreturn]] void throwRTLZZ(std::string message) {
    throw RTLZZException(std::move(message));
}

[[noreturn]] void throwRTLZZ(ErrorContext context, std::string message) {
    throw RTLZZException(std::move(context), std::move(message));
}

} // namespace pred
