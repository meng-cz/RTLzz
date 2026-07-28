#pragma once

#include "s0clang18/s008record.hpp"

#include <clang/AST/Decl.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s0clang18 {

enum class FunctionEntityKind {
    Top,
    Helper,
    FunctionTemplateSpecialization,
    Lambda,
    GenericLambdaSpecialization,
    Method,
};

struct FunctionInstanceKey {
    const clang::FunctionDecl* function_decl = nullptr;
    std::vector<long long> template_values;
    std::string stable_name;
};

struct FunctionEntity {
    SemanticEntityId id = -1;
    FunctionEntityKind kind = FunctionEntityKind::Helper;
    FunctionInstanceKey key;
    const clang::FunctionDecl* function_decl = nullptr;
    DebugLoc loc;
};

struct FunctionCallEdge {
    SemanticEntityId caller = -1;
    SemanticEntityId callee = -1;
    const clang::Expr* call_expr = nullptr;
    DebugLoc loc;
};

struct FunctionReachabilityGraph {
    SemanticEntityId top_function = -1;
    std::vector<FunctionEntity> functions;
    std::vector<FunctionCallEdge> call_edges;
    std::unordered_map<const clang::FunctionDecl*, SemanticEntityId> function_by_decl;
    std::unordered_map<std::string, SemanticEntityId> function_by_stable_name;
};

StepResult<FunctionReachabilityGraph> collectFunctionReachability(
    const Clang18Session& session,
    const TopFunctionSelection& top,
    const SemanticIndex& semantic_index,
    const ConstEvalContext& const_eval,
    const SourceLocPolicy& loc_policy = {});

const FunctionEntity* findFunctionEntity(const FunctionReachabilityGraph& graph,
                                         SemanticEntityId id);

} // namespace pred::s0clang18

