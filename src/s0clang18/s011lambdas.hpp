#pragma once

#include "s0clang18/s010templates.hpp"
#include "v2/V2Types.h"

#include <clang/AST/ExprCXX.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s0clang18 {

enum class LambdaCaptureKind {
    ByCopy,
    ByReference,
    This,
};

struct LambdaCapture {
    LambdaCaptureKind kind = LambdaCaptureKind::ByCopy;
    std::string source_name;
    std::string lowered_param_name;
    pred::v2::TypeInfo type;
    const clang::Decl* captured_decl = nullptr;
    DebugLoc loc;
};

struct LambdaInfo {
    SemanticEntityId function_id = -1;
    const clang::LambdaExpr* lambda_expr = nullptr;
    const clang::CXXMethodDecl* call_operator = nullptr;
    std::vector<LambdaCapture> captures;
    DebugLoc loc;
};

struct LambdaCaptureTable {
    std::vector<LambdaInfo> lambdas;
    std::unordered_map<SemanticEntityId, std::size_t> lambda_by_function;
};

StepResult<LambdaCaptureTable> resolveLambdaCaptures(
    const Clang18Session& session,
    const FunctionReachabilityGraph& reachability,
    const TypeLoweringContext& type_context,
    const SourceLocPolicy& loc_policy = {});

const LambdaInfo* findLambdaInfo(const LambdaCaptureTable& table,
                                 SemanticEntityId function_id);

} // namespace pred::s0clang18

