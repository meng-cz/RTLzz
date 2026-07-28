#pragma once

#include "s0clang18/s003ports.hpp"

#include <clang/AST/Decl.h>

#include <string>
#include <vector>

namespace pred::s0clang18 {

struct TopFunctionSelection {
    std::string requested_name;
    std::string resolved_name;
    const clang::FunctionDecl* function_decl = nullptr;
    DebugLoc loc;
};

struct TopFunctionCandidate {
    std::string name;
    const clang::FunctionDecl* function_decl = nullptr;
    DebugLoc loc;
};

StepResult<TopFunctionSelection> selectTopFunction(
    const Clang18Session& session,
    const std::string& top_pattern,
    const SourceLocPolicy& loc_policy = {});

std::vector<TopFunctionCandidate> collectTopFunctionCandidates(
    const Clang18Session& session,
    const std::string& top_pattern,
    const SourceLocPolicy& loc_policy = {});

} // namespace pred::s0clang18

