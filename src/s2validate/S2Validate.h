#pragma once

#include "ast/AST.h"
#include "debug/RTLZZException.h"

#include <optional>
#include <string>
#include <vector>

namespace pred::s2validate {

struct ValidateWarning {
    ErrorContext context;
    std::string message;
};

struct ValidateError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct ValidateOptions {
    bool debug_print = false;
};

struct ValidateResult {
    std::optional<ValidateError> error;
    std::vector<ValidateWarning> warnings;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

ValidateResult validateFunctionAST(const FunctionAST& function,
                                   const ValidateOptions& options = {});

void validateFunctionASTOrThrow(const FunctionAST& function,
                                const ValidateOptions& options = {});

} // namespace pred::s2validate
