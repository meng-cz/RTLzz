#pragma once

#include "s0clang18/s012expr.hpp"
#include "v2/V2AST.h"

#include <clang/AST/Stmt.h>

#include <vector>

namespace pred::s0clang18 {

struct StmtBuildContext {
    ExprBuildContext expr_context;
    SemanticEntityId current_function = -1;
};

struct StmtBuildResult {
    std::vector<pred::v2::StmtPtr> stmts;
    std::vector<Diagnostic> diagnostics;
};

StmtBuildResult buildStmt(const StmtBuildContext& context,
                          const clang::Stmt* stmt);

StmtBuildResult buildCompoundStmt(const StmtBuildContext& context,
                                  const clang::CompoundStmt* stmt);

StmtBuildResult buildFunctionBody(const StmtBuildContext& context,
                                  const clang::FunctionDecl* function_decl);

} // namespace pred::s0clang18

