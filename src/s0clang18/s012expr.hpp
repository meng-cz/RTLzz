#pragma once

#include "s0clang18/s011lambdas.hpp"
#include "v2/V2AST.h"

#include <clang/AST/Expr.h>

#include <string>
#include <vector>

namespace pred::s0clang18 {

struct ExprBuildContext {
    const Clang18Session* session = nullptr;
    const SemanticIndex* semantic_index = nullptr;
    const TypeLoweringContext* type_context = nullptr;
    const ConstEvalContext* const_eval = nullptr;
    const RecordMetadataSet* records = nullptr;
    const FunctionReachabilityGraph* reachability = nullptr;
    const TemplateSpecializationTable* templates = nullptr;
    const LambdaCaptureTable* lambdas = nullptr;
    SourceLocPolicy loc_policy;
};

struct ExprBuildResult {
    pred::v2::ExprPtr expr;
    std::vector<Diagnostic> diagnostics;
};

ExprBuildResult buildExpr(const ExprBuildContext& context,
                          const clang::Expr* expr);

ExprBuildResult buildLValueExpr(const ExprBuildContext& context,
                                const clang::Expr* expr);

std::string binaryOpcodeForClangExpr(const clang::Expr* expr);
std::string unaryOpcodeForClangExpr(const clang::Expr* expr);

} // namespace pred::s0clang18

