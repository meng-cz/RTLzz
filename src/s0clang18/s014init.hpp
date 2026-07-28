#pragma once

#include "s0clang18/s013stmt.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>

#include <optional>
#include <vector>

namespace pred::s0clang18 {

enum class InitForm {
    None,
    Copy,
    Direct,
    List,
    Aggregate,
    Designated,
    Value,
};

struct InitBuildInput {
    const clang::VarDecl* decl = nullptr;
    const clang::Expr* init_expr = nullptr;
    pred::v2::TypeInfo target_type;
    DebugLoc loc;
};

struct InitBuildResult {
    InitForm form = InitForm::None;
    std::optional<pred::v2::ExprPtr> init_expr;
    std::vector<pred::v2::ExprPtr> init_args;
    bool default_constructed = false;
    std::vector<Diagnostic> diagnostics;
};

InitBuildResult buildInitializer(const ExprBuildContext& context,
                                 const InitBuildInput& input);

InitBuildResult buildAggregateInitializer(const ExprBuildContext& context,
                                          const InitBuildInput& input);

bool hasSyntacticInitializer(const clang::VarDecl* decl);

} // namespace pred::s0clang18
