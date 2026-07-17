#pragma once

#include "v2/V2AST.h"

#include <optional>
#include <string>
#include <vector>

namespace pred::s0ast {

struct NativeBuildResult {
    std::optional<pred::v2::FunctionAST> function;
    std::string error;
};

NativeBuildResult buildV2ASTFromSource(
    const std::string& source_file,
    const std::string& top_function,
    const std::vector<std::string>& extra_args = {});

NativeBuildResult buildV2ASTFromSourceText(
    const std::string& source_name,
    const std::string& source_text,
    const std::string& top_function,
    const std::vector<std::string>& extra_args = {});

} // namespace pred::s0ast
