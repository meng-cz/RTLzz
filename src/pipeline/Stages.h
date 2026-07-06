#pragma once

#include "ast/AST.h"
#include "ir/CFG.h"
#include "ir/SSA.h"
#include "transform/LoopUnroll.h"
#include "transform/Normalize.h"

#include <optional>
#include <string>
#include <vector>

namespace pred::pipeline {

struct ParseConfig {
    std::string source_file;
    std::string top_function;
    std::vector<std::string> clang_args;
};

struct ParseResult {
    std::optional<FunctionAST> function;
    std::string error;
};

struct UnrollStageResult {
    std::optional<FunctionAST> function;
    std::string error;
};

struct InlineStageResult {
    std::optional<FunctionAST> function;
    std::string error;
};

struct CFGStageResult {
    CFG cfg;
    std::string error;
};

struct SSAStageResult {
    SSAProgram program;
    std::string error;
};

ParseResult parseSource(const ParseConfig& config);
UnrollStageResult unrollFunction(FunctionAST function,
                                 const UnrollConfig& config = {});
InlineStageResult inlineHelpersAndLambdas(FunctionAST function);
NormalizeResult normalizeFunction(const FunctionAST& function);
CFGStageResult buildControlFlow(const NormalizeResult& normalized);
SSAStageResult buildSSAForm(const CFGStageResult& cfg,
                            const NormalizeResult& normalized);

} // namespace pred::pipeline
