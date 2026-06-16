#include "normalize/NormalizeUtils.h"

#include <algorithm>
#include <utility>

namespace pred {

ExprPtr substituteInlineExpr(const ExprPtr& e,
                             const std::unordered_map<std::string, ExprPtr>& args) {
    if (!e) return nullptr;
    if (e->kind == ExprKind::VarRef) {
        auto it = args.find(e->var_name);
        if (it != args.end()) return cloneExpr(it->second);
    }
    auto r = std::make_shared<Expr>(*e);
    if (e->left) r->left = substituteInlineExpr(e->left, args);
    if (e->right) r->right = substituteInlineExpr(e->right, args);
    if (e->operand) r->operand = substituteInlineExpr(e->operand, args);
    if (e->array_base) r->array_base = substituteInlineExpr(e->array_base, args);
    if (e->index) r->index = substituteInlineExpr(e->index, args);
    if (e->struct_base) r->struct_base = substituteInlineExpr(e->struct_base, args);
    if (e->cast_expr) r->cast_expr = substituteInlineExpr(e->cast_expr, args);
    if (e->cond) r->cond = substituteInlineExpr(e->cond, args);
    if (e->then_expr) r->then_expr = substituteInlineExpr(e->then_expr, args);
    if (e->else_expr) r->else_expr = substituteInlineExpr(e->else_expr, args);
    if (e->base) r->base = substituteInlineExpr(e->base, args);
    if (e->value) r->value = substituteInlineExpr(e->value, args);
    r->args.clear();
    for (auto& a : e->args) r->args.push_back(substituteInlineExpr(a, args));
    r->parts.clear();
    for (auto& p : e->parts) r->parts.push_back(substituteInlineExpr(p, args));
    return r;
}

StmtPtr substituteInlineStmt(const StmtPtr& s,
                             const std::unordered_map<std::string, ExprPtr>& args) {
    if (!s) return nullptr;
    auto r = std::make_shared<Stmt>(*s);
    if (s->kind == StmtKind::Assign) {
        r->assign_target = substituteInlineExpr(s->assign_target, args);
        r->assign_value = substituteInlineExpr(s->assign_value, args);
    } else if (s->kind == StmtKind::Decl) {
        auto it = args.find(s->decl_name);
        if (it != args.end() && it->second && it->second->kind == ExprKind::VarRef) {
            r->decl_name = it->second->var_name;
        }
        if (s->decl_init.has_value()) r->decl_init = substituteInlineExpr(s->decl_init.value(), args);
    } else if (s->kind == StmtKind::If) {
        r->if_cond = substituteInlineExpr(s->if_cond, args);
        r->if_then = substituteInlineStmts(s->if_then, args);
        r->if_else = substituteInlineStmts(s->if_else, args);
    } else if (s->kind == StmtKind::Block) {
        r->block_stmts = substituteInlineStmts(s->block_stmts, args);
    } else if (s->kind == StmtKind::Return) {
        if (s->return_value.has_value()) r->return_value = substituteInlineExpr(s->return_value.value(), args);
    } else if (s->kind == StmtKind::ExprStmt) {
        r->expr_stmt = substituteInlineExpr(s->expr_stmt, args);
    }
    return r;
}

std::vector<StmtPtr> substituteInlineStmts(const std::vector<StmtPtr>& stmts,
                                           const std::unordered_map<std::string, ExprPtr>& args) {
    std::vector<StmtPtr> out;
    for (auto& s : stmts) {
        auto r = substituteInlineStmt(s, args);
        if (r) out.push_back(r);
    }
    return out;
}

void collectVarRefs(const ExprPtr& e, std::vector<std::string>& order) {
    if (!e) return;
    if (e->kind == ExprKind::VarRef) {
        if (std::find(order.begin(), order.end(), e->var_name) == order.end()) {
            order.push_back(e->var_name);
        }
        return;
    }
    collectVarRefs(e->left, order);
    collectVarRefs(e->right, order);
    collectVarRefs(e->operand, order);
    collectVarRefs(e->array_base, order);
    collectVarRefs(e->index, order);
    collectVarRefs(e->struct_base, order);
    collectVarRefs(e->cast_expr, order);
    collectVarRefs(e->cond, order);
    collectVarRefs(e->then_expr, order);
    collectVarRefs(e->else_expr, order);
    collectVarRefs(e->base, order);
    collectVarRefs(e->value, order);
    for (auto& a : e->args) collectVarRefs(a, order);
    for (auto& p : e->parts) collectVarRefs(p, order);
}

void collectStmtVarRefs(const StmtPtr& s, std::vector<std::string>& order) {
    if (!s) return;
    collectVarRefs(s->assign_target, order);
    collectVarRefs(s->assign_value, order);
    if (s->decl_init.has_value()) collectVarRefs(s->decl_init.value(), order);
    collectVarRefs(s->if_cond, order);
    for (auto& child : s->if_then) collectStmtVarRefs(child, order);
    for (auto& child : s->if_else) collectStmtVarRefs(child, order);
    for (auto& child : s->block_stmts) collectStmtVarRefs(child, order);
    collectVarRefs(s->switch_expr, order);
    for (auto& c : s->switch_cases) {
        if (c.value.has_value()) collectVarRefs(c.value.value(), order);
        for (auto& child : c.body) collectStmtVarRefs(child, order);
    }
    collectVarRefs(s->for_cond, order);
    collectVarRefs(s->for_step, order);
    if (s->for_init) collectStmtVarRefs(s->for_init, order);
    for (auto& child : s->for_body) collectStmtVarRefs(child, order);
    if (s->return_value.has_value()) collectVarRefs(s->return_value.value(), order);
    collectVarRefs(s->expr_stmt, order);
}

bool isTrueLiteral(const ExprPtr& e) {
    return e && e->kind == ExprKind::Literal &&
           (e->literal_value == "true" || e->literal_value == "1");
}

bool isFalseLiteral(const ExprPtr& e) {
    return e && e->kind == ExprKind::Literal &&
           (e->literal_value == "false" || e->literal_value == "0");
}

ExprPtr notExpr(ExprPtr e) {
    if (isTrueLiteral(e)) return make_literal("false", make_hw_type("bool", 1, false));
    if (isFalseLiteral(e)) return make_literal("true", make_hw_type("bool", 1, false));
    return make_unary("!", std::move(e), make_hw_type("bool", 1, false));
}

ExprPtr andExpr(ExprPtr a, ExprPtr b) {
    if (isFalseLiteral(a) || isFalseLiteral(b)) {
        return make_literal("false", make_hw_type("bool", 1, false));
    }
    if (isTrueLiteral(a)) return b;
    if (isTrueLiteral(b)) return a;
    return make_binary("&&", std::move(a), std::move(b), make_hw_type("bool", 1, false));
}

StmtPtr guardStmt(ExprPtr guard, StmtPtr stmt) {
    if (!stmt) return nullptr;
    if (isTrueLiteral(guard)) return stmt;
    if (isFalseLiteral(guard)) return nullptr;
    auto wrapped = std::make_shared<Stmt>();
    wrapped->kind = StmtKind::If;
    wrapped->if_cond = std::move(guard);
    wrapped->if_then.push_back(std::move(stmt));
    return wrapped;
}

bool isVoidReturnStmt(const StmtPtr& s) {
    return s && s->kind == StmtKind::Return && !s->return_value.has_value();
}

bool containsReturnStmt(const std::vector<StmtPtr>& body) {
    return std::any_of(body.begin(), body.end(), [](const StmtPtr& s) {
        if (!s) return false;
        if (s->kind == StmtKind::Return) return true;
        if (containsReturnStmt(s->if_then) || containsReturnStmt(s->if_else)) return true;
        if (containsReturnStmt(s->block_stmts) || containsReturnStmt(s->for_body)) return true;
        if (s->for_init && containsReturnStmt({s->for_init})) return true;
        for (const auto& c : s->switch_cases) {
            if (containsReturnStmt(c.body)) return true;
        }
        return false;
    });
}

} // namespace pred
