#include "backend/pipeline_valid.hpp"

#include <functional>
#include <stdexcept>

namespace pred::pipeline {
void prepareValidOutput(v2::FunctionAST& function, const Options& options) {
    if (options.input_valid_name.empty()) return;
    auto port = [&](const std::string& name, v2::ParamDirection direction) -> const v2::ParamDecl& {
        for (const auto& p : function.params) if (p.name == name) {
            if (p.direction != direction || !v2::is_bool_type_info(p.type) || !p.type.array_dims.empty())
                throw std::runtime_error("pipeline: valid port must be a scalar bool with the configured direction: " + name);
            return p;
        }
        throw std::runtime_error("pipeline: unknown valid port: " + name);
    };
    port(options.input_valid_name, v2::ParamDirection::Input);
    const auto output = port(options.output_valid_name, v2::ParamDirection::Output);
    std::function<void(const v2::ExprPtr&)> expr;
    std::function<void(const v2::StmtPtr&)> stmt;
    expr = [&](const v2::ExprPtr& e) {
        if (!e) return;
        if (e->kind == v2::ExprKind::VarRef && e->var_name == output.name)
            throw std::runtime_error("pipeline: valid output is owned by RTLZZ and cannot be read or assigned: " + output.name);
        for (const auto& child : {e->left, e->right, e->operand, e->array_base, e->index,
             e->struct_base, e->cast_expr, e->cond, e->then_expr, e->else_expr, e->base, e->value}) expr(child);
        for (const auto& child : e->args) expr(child);
        for (const auto& child : e->parts) expr(child);
    };
    stmt = [&](const v2::StmtPtr& s) {
        if (!s) return;
        for (const auto& e : {s->assign_target, s->assign_value, s->if_cond, s->for_cond,
             s->for_step, s->while_cond, s->switch_expr, s->expr_stmt}) expr(e);
        if (s->decl_init) expr(*s->decl_init);
        if (s->return_value) expr(*s->return_value);
        for (const auto& e : s->decl_init_args) expr(e);
        stmt(s->for_init);
        for (const auto* block : {&s->if_then, &s->if_else, &s->for_body, &s->while_body, &s->block_stmts})
            for (const auto& child : *block) stmt(child);
        for (const auto& c : s->switch_cases) {
            if (c.value) expr(*c.value);
            for (const auto& child : c.body) stmt(child);
        }
    };
    for (const auto& s : function.body) stmt(s);
    auto placeholder = std::make_shared<v2::Stmt>();
    placeholder->kind = v2::StmtKind::Assign;
    placeholder->assign_target = v2::make_var(output.name, output.type);
    placeholder->assign_value = v2::make_literal("false", v2::make_bool_type());
    // Backend ownership is explicit; normal data outputs remain uninitialized.
    function.body.insert(function.body.begin(), std::move(placeholder));
}
}
