#pragma once

#include "ast/AST.h"
#include "debug/RTLZZException.h"
#include "s1apinorm/S1NormedAST.h"

#include <optional>
#include <string>
#include <vector>

namespace pred::s1apinorm {

struct APINormWarning {
    ErrorContext context;
    std::string message;
};

struct APINormError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct APINormOptions {
    bool debug_print = false;
};

struct APINormSummary {
    std::string function_name;
    int normalized_calls = 0;
    int normalized_writes = 0;
};

struct APINormResult {
    std::optional<S1FunctionAST> function;
    std::optional<APINormError> error;
    std::vector<APINormWarning> warnings;
    std::vector<APINormSummary> summaries;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

APINormResult normalizeAPIs(
    const FunctionAST& function,
    const APINormOptions& options = {});

S1FunctionAST normalizeAPIsOrThrow(
    const FunctionAST& function,
    const APINormOptions& options = {});

std::string debugPrint(const S1FunctionAST& function,
                       const std::vector<APINormSummary>& summaries);

} // namespace pred::s1apinorm
