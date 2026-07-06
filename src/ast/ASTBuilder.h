#pragma once

#include "ast/AST.h"
#include <string>
#include <optional>

namespace pred {

struct BuildResult {
    std::optional<FunctionAST> function;
    std::string error;
};

BuildResult buildASTFromSource(const std::string& source_file,
                               const std::string& top_function,
                               const std::vector<std::string>& extra_args = {});

BuildResult buildASTFromSourceText(const std::string& source_name,
                                   const std::string& source_text,
                                   const std::string& top_function,
                                   const std::vector<std::string>& extra_args = {});

} // namespace pred
