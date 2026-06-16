#include "normalize/NormalizeUtils.h"

namespace pred {

namespace {

TypeInfo boolType() {
    return make_hw_type("bool", 1, false);
}

StmtPtr cloneStmtNoSubstitution(const StmtPtr& stmt) {
    static const std::unordered_map<std::string, ExprPtr> no_args;
    return substituteInlineStmt(stmt, no_args);
}

struct LocalReturnResult {
    std::vector<StmtPtr> stmts;
    ExprPtr alive;
};

ExprPtr mergeAliveAfterIf(const ExprPtr& cond,
                          const ExprPtr& then_alive,
                          const ExprPtr& else_alive) {
    if (isTrueLiteral(then_alive) && isTrueLiteral(else_alive)) {
        return make_literal("true", boolType());
    }
    if (isFalseLiteral(then_alive) && isFalseLiteral(else_alive)) {
        return make_literal("false", boolType());
    }
    return make_ternary(cloneExpr(cond),
                        cloneExpr(then_alive),
                        cloneExpr(else_alive),
                        boolType());
}

LocalReturnResult localizeReturnSeq(const std::vector<StmtPtr>& stmts,
                                    const std::string& callee,
                                    std::string& error) {
    LocalReturnResult result;
    result.alive = make_literal("true", boolType());

    for (const auto& stmt : stmts) {
        if (!stmt || !error.empty()) continue;

        if (stmt->kind == StmtKind::Return) {
            if (stmt->return_value.has_value()) {
                error = "Unsupported non-void return in procedural inline call '" + callee + "'";
                return {};
            }
            result.alive = make_literal("false", boolType());
            continue;
        }

        if (isFalseLiteral(result.alive)) {
            continue;
        }

        if (stmt->kind == StmtKind::If) {
            auto then_result = localizeReturnSeq(stmt->if_then, callee, error);
            if (!error.empty()) return {};

            LocalReturnResult else_result;
            if (!stmt->if_else.empty()) {
                else_result = localizeReturnSeq(stmt->if_else, callee, error);
                if (!error.empty()) return {};
            } else {
                else_result.alive = make_literal("true", boolType());
            }

            if (!then_result.stmts.empty() || !else_result.stmts.empty()) {
                auto rewritten = cloneStmtNoSubstitution(stmt);
                rewritten->if_then = std::move(then_result.stmts);
                rewritten->if_else = std::move(else_result.stmts);
                auto guarded = guardStmt(cloneExpr(result.alive), std::move(rewritten));
                if (guarded) result.stmts.push_back(std::move(guarded));
            }

            auto branch_alive = mergeAliveAfterIf(stmt->if_cond,
                                                  then_result.alive,
                                                  else_result.alive);
            result.alive = andExpr(std::move(result.alive), std::move(branch_alive));
            continue;
        }

        if (stmt->kind == StmtKind::Block) {
            auto block_result = localizeReturnSeq(stmt->block_stmts, callee, error);
            if (!error.empty()) return {};
            if (!block_result.stmts.empty()) {
                auto rewritten = cloneStmtNoSubstitution(stmt);
                rewritten->block_stmts = std::move(block_result.stmts);
                auto guarded = guardStmt(cloneExpr(result.alive), std::move(rewritten));
                if (guarded) result.stmts.push_back(std::move(guarded));
            }
            result.alive = andExpr(std::move(result.alive), std::move(block_result.alive));
            continue;
        }

        if (containsReturnStmt({stmt})) {
            error = "Unsupported nested return shape in procedural inline call '" + callee + "'";
            return {};
        }

        auto guarded = guardStmt(cloneExpr(result.alive), cloneStmtNoSubstitution(stmt));
        if (guarded) result.stmts.push_back(std::move(guarded));
    }

    return result;
}

} // namespace

std::vector<StmtPtr> localizeProcedureReturns(const std::vector<StmtPtr>& stmts,
                                              const std::string& callee,
                                              std::string& error) {
    return localizeReturnSeq(stmts, callee, error).stmts;
}

} // namespace pred
