#pragma once

#include "s0clang18/s014init.hpp"

#include <clang/AST/Expr.h>

#include <optional>
#include <string>
#include <vector>

namespace pred::s0clang18 {

enum class BoundCallKind {
    Unknown,
    Helper,
    TemplateHelper,
    Lambda,
    GenericLambda,
    MemberHelper,
    FixintAPI,
    Unsupported,
};

struct BoundCall {
    BoundCallKind kind = BoundCallKind::Unknown;
    std::string stable_callee_name;
    SemanticEntityId function_id = -1;
    std::string api_name;
    std::vector<long long> template_values;
    const clang::Expr* receiver_expr = nullptr;
    const clang::FunctionDecl* callee_decl = nullptr;
    DebugLoc loc;
};

struct CallBindingResult {
    std::optional<BoundCall> call;
    std::vector<Diagnostic> diagnostics;
};

CallBindingResult bindCallExpr(const ExprBuildContext& context,
                               const clang::CallExpr* expr);

CallBindingResult bindCXXMemberCallExpr(const ExprBuildContext& context,
                                        const clang::CXXMemberCallExpr* expr);

CallBindingResult bindCXXOperatorCallExpr(const ExprBuildContext& context,
                                          const clang::CXXOperatorCallExpr* expr);

} // namespace pred::s0clang18

