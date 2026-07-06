#pragma once

#include "ast/AST.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pred {

struct NormalizeResult {
    std::vector<StmtPtr> body;
    std::unordered_map<std::string, TypeInfo> symbols;
    std::unordered_map<std::string, TypeInfo> ssa_seed_symbols;
    std::unordered_map<std::string, std::vector<std::string>> lookup_tables;
    std::unordered_map<std::string, std::string> param_directions;
    std::unordered_map<std::string, std::string> output_default_reasons;
    std::unordered_map<std::string, std::string> output_paired_controls;
    std::vector<std::string> output_params;
    std::string error;
};

struct InlineResult {
    std::vector<StmtPtr> body;
    std::string error;
};

// Expands lambda/helper calls at the AST statement/expression level. This pass
// intentionally runs before the predicate-friendly normalization that flattens
// arrays and structs.
InlineResult inlineHelpersAndLambdas(const FunctionAST& func,
                                     const std::vector<StmtPtr>& body);

// Checks the supported C++ subset, validates bit widths and assignment
// coverage, then rewrites field accesses and dynamic array accesses into
// flat predicate-friendly expressions.
NormalizeResult normalizeFunction(const FunctionAST& func,
                                  const std::vector<StmtPtr>& body);

} // namespace pred
