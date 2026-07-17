#pragma once

#include "debug/RTLZZException.h"
#include "s1apinorm/S1NormedAST.h"

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

ValidateResult validateFunctionAST(const s1apinorm::S1FunctionAST& function,
                                   const ValidateOptions& options = {});

void validateFunctionASTOrThrow(const s1apinorm::S1FunctionAST& function,
                                const ValidateOptions& options = {});

} // namespace pred::s2validate
