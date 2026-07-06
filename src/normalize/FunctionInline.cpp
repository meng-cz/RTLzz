#include "transform/Normalize.h"
#include "normalize/NormalizeUtils.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <unordered_set>
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

namespace {

TypeInfo boolType() {
    return make_hw_type("bool", 1, false);
}

ExprPtr orInlineExpr(ExprPtr lhs, ExprPtr rhs) {
    if (!lhs) return rhs;
    if (!rhs) return lhs;
    if (isTrueLiteral(lhs) || isTrueLiteral(rhs)) return make_literal("true", boolType());
    if (isFalseLiteral(lhs)) return rhs;
    if (isFalseLiteral(rhs)) return lhs;
    return make_binary("||", std::move(lhs), std::move(rhs), boolType());
}

std::string normalizeTypeName(std::string name) {
    if (name.rfind("struct ", 0) == 0) name = name.substr(7);
    if (name.rfind("class ", 0) == 0) name = name.substr(6);
    return name;
}

std::string lambdaReturnFromOperatorType(const TypeInfo& type) {
    std::string name = type.name;
    auto arrow = name.find("->");
    if (arrow == std::string::npos) return "";
    std::string ret = name.substr(arrow + 2);
    auto comment = ret.find("//");
    if (comment != std::string::npos) ret = ret.substr(0, comment);
    ret.erase(0, ret.find_first_not_of(" \t"));
    auto end = ret.find_last_not_of(" \t");
    if (end == std::string::npos) return "";
    ret.erase(end + 1);
    return normalizeTypeName(ret);
}

class InlinePass {
public:
    explicit InlinePass(const FunctionAST& func) {
        for (const auto& helper : func.helpers) {
            if (helper) helpers_[helper->name] = helper.get();
        }
        for (const auto& item : func.lambdas) {
            if (item.second) lambdas_[item.first] = item.second.get();
        }
        struct_fields_ = func.struct_fields;
    }

    InlineResult run(const std::vector<StmtPtr>& body) {
        InlineResult result;
        result.body = rewriteStmts(body);
        result.error = error_;
        return result;
    }

private:
    struct ExprFlow {
        std::unordered_map<std::string, ExprPtr> values;
        std::unordered_map<std::string, TypeInfo> types;
        ExprPtr active = make_literal("true", boolType());
        ExprPtr return_value;
        ExprPtr return_guard;
    };

    std::unordered_map<std::string, const FunctionAST*> helpers_;
    std::unordered_map<std::string, const FunctionAST*> lambdas_;
    std::unordered_map<std::string, std::vector<StructFieldInfo>> struct_fields_;
    int inline_counter_ = 0;
    std::string error_;

    const std::vector<StructFieldInfo>* findStructFields(const TypeInfo& type) const {
        std::string canonical = canonicalStructName(
            type.struct_name.empty() ? type.name : type.struct_name);
        for (const auto& key : {type.struct_name, type.name, canonical,
                                std::string("struct ") + canonical}) {
            if (key.empty()) continue;
            auto it = struct_fields_.find(key);
            if (it != struct_fields_.end()) return &it->second;
        }
        return nullptr;
    }

    int flattenedWidth(const TypeInfo& type) const {
        if (type.width > 0) return type.width;
        const auto* fields = findStructFields(type);
        if (!fields || fields->empty()) return 0;
        int width = 0;
        for (const auto& field : *fields) {
            int field_width = flattenedWidth(field.type);
            if (field_width <= 0) return 0;
            width += field_width;
        }
        return width;
    }

    std::optional<std::pair<std::string, TypeInfo>>
    inlineLocalFieldTarget(const ExprPtr& target, const ExprFlow& flow) const {
        if (!target || target->kind != ExprKind::FieldAccess || !target->struct_base) {
            return std::nullopt;
        }
        const std::string base = baseName(target->struct_base);
        if (base.empty()) return std::nullopt;
        auto type_it = flow.types.find(base);
        if (type_it == flow.types.end()) return std::nullopt;
        const auto* fields = findStructFields(type_it->second);
        if (!fields) return std::nullopt;
        for (const auto& field : *fields) {
            if (field.name == target->field_name) {
                return std::make_pair(base + "_" + field.name, field.type);
            }
        }
        return std::nullopt;
    }

    ExprPtr packInlineStructLocal(const std::string& name,
                                  const TypeInfo& type,
                                  const ExprFlow& flow,
                                  const std::string& callee) {
        const auto* fields = findStructFields(type);
        if (!fields || fields->empty()) return nullptr;
        std::vector<ExprPtr> parts;
        for (const auto& field : *fields) {
            const std::string flat = name + "_" + field.name;
            ExprPtr part;
            if (findStructFields(field.type)) {
                part = packInlineStructLocal(flat, field.type, flow, callee);
            } else {
                auto value_it = flow.values.find(flat);
                if (value_it == flow.values.end()) {
                    error_ = "Helper function '" + callee +
                             "' returns struct local '" + name +
                             "' with uninitialized field '" + field.name + "'";
                    return nullptr;
                }
                part = castIfWidthChanges(cloneExpr(value_it->second), field.type);
            }
            if (!part || !error_.empty()) return nullptr;
            parts.push_back(std::move(part));
        }
        std::reverse(parts.begin(), parts.end());
        return make_concat(std::move(parts));
    }

    bool storeInlineStructLocal(const std::string& name,
                                const TypeInfo& type,
                                const ExprPtr& packed,
                                ExprFlow& flow,
                                const std::string& callee,
                                int& offset) {
        const auto* fields = findStructFields(type);
        if (!fields || fields->empty()) return false;
        for (const auto& field : *fields) {
            const std::string flat = name + "_" + field.name;
            if (findStructFields(field.type)) {
                flow.types[flat] = field.type;
                if (!storeInlineStructLocal(flat, field.type, packed, flow, callee, offset)) {
                    return false;
                }
                continue;
            }
            int field_width = flattenedWidth(field.type);
            if (field_width <= 0) {
                error_ = "Helper function '" + callee +
                         "' assigns struct field with unknown width '" + flat + "'";
                return false;
            }
            TypeInfo leaf_type = field.type;
            leaf_type.width = field_width;
            flow.types[flat] = leaf_type;
            flow.values[flat] = make_slice(cloneExpr(packed),
                                           offset + field_width - 1,
                                           offset,
                                           leaf_type);
            offset += field_width;
        }
        return true;
    }

    std::string returnedStructLocalName(const ExprPtr& expr, const ExprFlow& flow) const {
        if (!expr) return "";
        if (expr->kind == ExprKind::VarRef) {
            auto type_it = flow.types.find(expr->var_name);
            if (type_it != flow.types.end() && findStructFields(type_it->second)) {
                return expr->var_name;
            }
            return "";
        }
        if (expr->kind == ExprKind::Cast) {
            return returnedStructLocalName(expr->cast_expr, flow);
        }
        if ((expr->kind == ExprKind::ZExt || expr->kind == ExprKind::SExt ||
             expr->kind == ExprKind::Trunc) && expr->base) {
            return returnedStructLocalName(expr->base, flow);
        }
        if (expr->kind == ExprKind::Call && expr->args.size() == 1) {
            return returnedStructLocalName(expr->args.front(), flow);
        }
        return "";
    }

    const FunctionAST* resolveCallee(std::string& callee,
                                     const TypeInfo& call_type,
                                     std::size_t arg_count) {
        if (callee.rfind("__unsupported_operator_call_receiver", 0) == 0 &&
            arg_count == 0) {
            std::string expected_return = lambdaReturnFromOperatorType(call_type);
            std::string match;
            if (!expected_return.empty()) {
                for (const auto& [name, lambda] : lambdas_) {
                    if (name.rfind("__unsupported_operator_call_receiver", 0) == 0) continue;
                    std::string lambda_return = normalizeTypeName(lambda->return_type.name);
                    if (lambda_return.empty() && !lambda->return_type.struct_name.empty()) {
                        lambda_return = normalizeTypeName(lambda->return_type.struct_name);
                    }
                    if (lambda_return == expected_return) {
                        if (!match.empty()) {
                            error_ = "Ambiguous hidden lambda operator() call returning '" +
                                     expected_return + "'";
                            return nullptr;
                        }
                        match = name;
                    }
                }
            }
            if (match.empty()) {
                error_ = "Unsupported hidden operator() call without recoverable lambda receiver";
                return nullptr;
            }
            callee = match;
        }
        if (callee.rfind("__unsupported_operator_call_receiver", 0) == 0) {
            error_ = "Unsupported hidden operator() call without recoverable lambda receiver";
            return nullptr;
        }
        auto helper_it = helpers_.find(callee);
        if (helper_it != helpers_.end()) return helper_it->second;
        auto lambda_it = lambdas_.find(callee);
        if (lambda_it != lambdas_.end()) return lambda_it->second;
        return nullptr;
    }

    bool isInlineCallee(const ExprPtr& call) {
        if (!call || call->kind != ExprKind::Call) return false;
        std::string callee = call->callee;
        return resolveCallee(callee, call->type, call->args.size()) != nullptr && error_.empty();
    }

    void collectLocalRenames(const std::vector<StmtPtr>& statements,
                             const std::vector<std::string>& parameter_names,
                             int inline_id,
                             std::unordered_map<std::string, ExprPtr>& substitutions) {
        for (const auto& stmt : statements) {
            if (!stmt) continue;
            if (stmt->kind == StmtKind::Decl && !stmt->decl_type.is_static &&
                stmt->decl_type.init_values.empty() &&
                std::find(parameter_names.begin(), parameter_names.end(), stmt->decl_name) ==
                    parameter_names.end()) {
                substitutions.emplace(
                    stmt->decl_name,
                    make_var(stmt->decl_name + "__inl_" + std::to_string(inline_id),
                             stmt->decl_type));
            }
            collectLocalRenames(stmt->if_then, parameter_names, inline_id, substitutions);
            collectLocalRenames(stmt->if_else, parameter_names, inline_id, substitutions);
            collectLocalRenames(stmt->block_stmts, parameter_names, inline_id, substitutions);
            collectLocalRenames(stmt->for_body, parameter_names, inline_id, substitutions);
            collectLocalRenames(stmt->while_body, parameter_names, inline_id, substitutions);
            if (stmt->for_init) {
                collectLocalRenames({stmt->for_init}, parameter_names, inline_id, substitutions);
            }
            for (const auto& clause : stmt->switch_cases) {
                collectLocalRenames(clause.body, parameter_names, inline_id, substitutions);
            }
        }
    }

    void appendReturn(ExprFlow& flow, ExprPtr guard, ExprPtr value) {
        if (!flow.return_value) {
            flow.return_value = std::move(value);
            flow.return_guard = std::move(guard);
            return;
        }
        TypeInfo result_type = flow.return_value->type.width > 0
            ? flow.return_value->type
            : value->type;
        flow.return_value = make_ternary(cloneExpr(guard), std::move(value),
                                         std::move(flow.return_value), result_type);
        flow.return_guard = orInlineExpr(std::move(flow.return_guard), std::move(guard));
    }

    ExprPtr substituteFlowExpr(const ExprPtr& expr, const ExprFlow& flow) {
        if (!expr) return nullptr;
        if (expr->kind == ExprKind::VarRef) {
            auto it = flow.values.find(expr->var_name);
            if (it != flow.values.end()) return cloneExpr(it->second);
        }
        if (expr->kind == ExprKind::FieldAccess &&
            expr->struct_base) {
            const std::string base = baseName(expr->struct_base);
            if (!base.empty()) {
                const std::string flat = base + "_" + expr->field_name;
                auto it = flow.values.find(flat);
                if (it != flow.values.end()) return cloneExpr(it->second);
                auto base_it = flow.values.find(base);
                if (base_it != flow.values.end()) {
                    return make_field_access(cloneExpr(base_it->second),
                                             expr->field_name,
                                             expr->type);
                }
            }
        }
        auto r = std::make_shared<Expr>(*expr);
        if (expr->left) r->left = substituteFlowExpr(expr->left, flow);
        if (expr->right) r->right = substituteFlowExpr(expr->right, flow);
        if (expr->operand) r->operand = substituteFlowExpr(expr->operand, flow);
        if (expr->array_base) r->array_base = substituteFlowExpr(expr->array_base, flow);
        if (expr->index) r->index = substituteFlowExpr(expr->index, flow);
        if (expr->struct_base) r->struct_base = substituteFlowExpr(expr->struct_base, flow);
        if (expr->cast_expr) r->cast_expr = substituteFlowExpr(expr->cast_expr, flow);
        if (expr->cond) r->cond = substituteFlowExpr(expr->cond, flow);
        if (expr->then_expr) r->then_expr = substituteFlowExpr(expr->then_expr, flow);
        if (expr->else_expr) r->else_expr = substituteFlowExpr(expr->else_expr, flow);
        if (expr->base) r->base = substituteFlowExpr(expr->base, flow);
        if (expr->value) r->value = substituteFlowExpr(expr->value, flow);
        r->args.clear();
        for (const auto& arg : expr->args) r->args.push_back(substituteFlowExpr(arg, flow));
        r->parts.clear();
        for (const auto& part : expr->parts) r->parts.push_back(substituteFlowExpr(part, flow));
        return r;
    }

    ExprPtr rewriteInlineExpr(const ExprPtr& expr, const ExprFlow& flow) {
        return rewriteExpr(substituteFlowExpr(expr, flow));
    }

    bool lowerExprSequence(const std::vector<StmtPtr>& statements,
                           const std::string& callee,
                           ExprFlow& flow) {
        for (const auto& stmt : statements) {
            if (!stmt || isFalseLiteral(flow.active)) continue;
            switch (stmt->kind) {
            case StmtKind::Decl: {
                flow.types[stmt->decl_name] = stmt->decl_type;
                if (!stmt->decl_init.has_value()) {
                    if (const auto* fields = findStructFields(stmt->decl_type)) {
                        for (const auto& field : *fields) {
                            flow.types[stmt->decl_name + "_" + field.name] = field.type;
                        }
                        break;
                    }
                    error_ = "Helper function '" + callee +
                             "' declares uninitialized local '" + stmt->decl_name + "'";
                    return false;
                }
                auto value = rewriteInlineExpr(stmt->decl_init.value(), flow);
                if (!value || !error_.empty()) return false;
                if (findStructFields(stmt->decl_type)) {
                    int packed_width = flattenedWidth(stmt->decl_type);
                    if (packed_width <= 0) {
                        error_ = "Helper function '" + callee +
                                 "' declares struct local with unknown flattened width '" +
                                 stmt->decl_name + "'";
                        return false;
                    }
                    ExprPtr packed = value;
                    if (packed->type.width != packed_width) {
                        packed = castIfWidthChanges(packed,
                                                    make_hw_type("UInt", packed_width, false));
                    }
                    int offset = 0;
                    if (!storeInlineStructLocal(stmt->decl_name, stmt->decl_type,
                                                packed, flow, callee, offset)) {
                        return false;
                    }
                    break;
                }
                flow.values[stmt->decl_name] = castIfWidthChanges(value, stmt->decl_type);
                break;
            }
            case StmtKind::Assign: {
                std::string name;
                TypeInfo type;
                bool require_existing = true;
                if (stmt->assign_target && stmt->assign_target->kind == ExprKind::VarRef) {
                    name = stmt->assign_target->var_name;
                    auto type_it = flow.types.find(name);
                    if (type_it != flow.types.end()) type = type_it->second;
                } else if (auto field = inlineLocalFieldTarget(stmt->assign_target, flow)) {
                    name = field->first;
                    type = field->second;
                    flow.types[name] = type;
                    require_existing = false;
                } else {
                    error_ = "Helper function '" + callee +
                             "' expression inline supports assignments only to local/value variables";
                    return false;
                }
                auto old_it = flow.values.find(name);
                if (require_existing && old_it == flow.values.end()) {
                    error_ = "Helper function '" + callee +
                             "' assigns unknown local/value variable '" + name + "'";
                    return false;
                }
                auto value = rewriteInlineExpr(stmt->assign_value, flow);
                if (!value || !error_.empty()) return false;
                if (type.width <= 0) {
                    type = flow.types.count(name) ? flow.types[name] :
                           (old_it != flow.values.end() ? old_it->second->type : value->type);
                }
                if (findStructFields(type)) {
                    int packed_width = flattenedWidth(type);
                    if (packed_width <= 0) {
                        error_ = "Helper function '" + callee +
                                 "' assigns struct local with unknown flattened width '" +
                                 name + "'";
                        return false;
                    }
                    ExprPtr packed = value;
                    if (packed->type.width != packed_width) {
                        packed = castIfWidthChanges(packed,
                                                    make_hw_type("UInt", packed_width, false));
                    }
                    int offset = 0;
                    if (!storeInlineStructLocal(name, type, packed, flow, callee, offset)) {
                        return false;
                    }
                    break;
                }
                value = castIfWidthChanges(value, type);
                if (isTrueLiteral(flow.active) || old_it == flow.values.end()) {
                    flow.values[name] = std::move(value);
                } else {
                    flow.values[name] = make_ternary(cloneExpr(flow.active), std::move(value),
                                                     cloneExpr(old_it->second), type);
                }
                break;
            }
            case StmtKind::If: {
                auto condition = rewriteInlineExpr(stmt->if_cond, flow);
                if (!condition || !error_.empty()) return false;

                ExprFlow then_flow = flow;
                then_flow.active = andExpr(cloneExpr(flow.active), cloneExpr(condition));
                ExprFlow else_flow = flow;
                else_flow.active = andExpr(cloneExpr(flow.active), notExpr(cloneExpr(condition)));

                if (!lowerExprSequence(stmt->if_then, callee, then_flow)) return false;
                if (!lowerExprSequence(stmt->if_else, callee, else_flow)) return false;

                for (auto& [name, old_value] : flow.values) {
                    auto then_it = then_flow.values.find(name);
                    auto else_it = else_flow.values.find(name);
                    if (then_it == then_flow.values.end() || else_it == else_flow.values.end()) {
                        error_ = "Helper function '" + callee +
                                 "' has an uninitialized local after conditional assignment: '" +
                                 name + "'";
                        return false;
                    }
                    TypeInfo type = old_value && old_value->type.width > 0
                        ? old_value->type
                        : flow.types[name];
                    flow.values[name] = make_ternary(cloneExpr(condition),
                                                     cloneExpr(then_it->second),
                                                     cloneExpr(else_it->second),
                                                     type);
                }
                for (const auto& [name, then_value] : then_flow.values) {
                    if (flow.values.count(name)) continue;
                    auto else_it = else_flow.values.find(name);
                    if (else_it == else_flow.values.end()) continue;
                    TypeInfo type = then_value && then_value->type.width > 0
                        ? then_value->type
                        : flow.types[name];
                    flow.values[name] = make_ternary(cloneExpr(condition),
                                                     cloneExpr(then_value),
                                                     cloneExpr(else_it->second),
                                                     type);
                }
                if (then_flow.return_value) {
                    appendReturn(flow, cloneExpr(then_flow.return_guard),
                                 cloneExpr(then_flow.return_value));
                }
                if (else_flow.return_value) {
                    appendReturn(flow, cloneExpr(else_flow.return_guard),
                                 cloneExpr(else_flow.return_value));
                }
                flow.active = orInlineExpr(std::move(then_flow.active), std::move(else_flow.active));
                break;
            }
            case StmtKind::Block: {
                std::vector<std::string> visible_names;
                visible_names.reserve(flow.values.size());
                for (const auto& [name, value] : flow.values) visible_names.push_back(name);
                if (!lowerExprSequence(stmt->block_stmts, callee, flow)) return false;
                std::unordered_set<std::string> visible(visible_names.begin(), visible_names.end());
                for (auto it = flow.values.begin(); it != flow.values.end();) {
                    if (!visible.count(it->first)) {
                        flow.types.erase(it->first);
                        it = flow.values.erase(it);
                    } else {
                        ++it;
                    }
                }
                break;
            }
            case StmtKind::Return: {
                if (!stmt->return_value.has_value()) {
                    error_ = "Helper function '" + callee +
                             "' used as expression contains a void return";
                    return false;
                }
                ExprPtr value;
                auto returned = stmt->return_value.value();
                std::string returned_local = returnedStructLocalName(returned, flow);
                if (!returned_local.empty()) {
                    auto type_it = flow.types.find(returned_local);
                    if (type_it != flow.types.end() && findStructFields(type_it->second)) {
                        value = packInlineStructLocal(returned_local,
                                                      type_it->second,
                                                      flow,
                                                      callee);
                    }
                }
                if (!value) value = rewriteInlineExpr(returned, flow);
                if (!value || !error_.empty()) return false;
                appendReturn(flow, cloneExpr(flow.active), std::move(value));
                flow.active = make_literal("false", boolType());
                break;
            }
            default:
                error_ = "Helper function '" + callee +
                         "' used as expression contains unsupported control flow or side effects";
                return false;
            }
        }
        return true;
    }

    ExprPtr inlineExpressionCall(const ExprPtr& call,
                                 const FunctionAST& helper,
                                 const std::string& callee) {
        std::vector<std::string> param_names;
        for (auto& p : helper.params) param_names.push_back(p.name);

        std::size_t arg_offset = call->args.size() == param_names.size() + 1 ? 1 : 0;
        if (param_names.size() != call->args.size() - arg_offset) {
            error_ = "Helper function '" + callee + "' argument count mismatch: params=" +
                     std::to_string(param_names.size()) + " args=" +
                     std::to_string(call->args.size()) + " offset=" +
                     std::to_string(arg_offset);
            return nullptr;
        }

        ExprFlow flow;
        for (std::size_t i = 0; i < helper.params.size(); ++i) {
            const auto& param = helper.params[i];
            flow.types[param.name] = param.type;
            auto arg = rewriteExpr(call->args[i + arg_offset]);
            if (!arg || !error_.empty()) return nullptr;
            if (const auto* fields = findStructFields(param.type)) {
                flow.values[param.name] = cloneExpr(arg);
                for (const auto& field : *fields) {
                    const std::string flat = param.name + "_" + field.name;
                    flow.types[flat] = field.type;
                    flow.values[flat] = make_field_access(cloneExpr(arg),
                                                          field.name,
                                                          field.type);
                }
            } else {
                flow.values[param.name] = arg;
            }
        }

        if (!lowerExprSequence(helper.body, callee, flow)) return nullptr;
        if (!flow.return_value) {
            error_ = "Helper function '" + callee + "' used as expression has no return value";
            return nullptr;
        }
        if (!isFalseLiteral(flow.active)) {
            error_ = "Helper function '" + callee +
                     "' does not return a value on every statically represented path";
            return nullptr;
        }
        return castIfWidthChanges(flow.return_value, call->type);
    }

    ExprPtr rewriteExpr(const ExprPtr& expr) {
        if (!expr || !error_.empty()) return nullptr;
        if (expr->kind == ExprKind::Call) {
            std::string callee = expr->callee;
            if (const FunctionAST* helper = resolveCallee(callee, expr->type, expr->args.size())) {
                return inlineExpressionCall(expr, *helper, callee);
            }
            if (!error_.empty()) return nullptr;
        }

        auto r = std::make_shared<Expr>(*expr);
        if (expr->left) r->left = rewriteExpr(expr->left);
        if (expr->right) r->right = rewriteExpr(expr->right);
        if (expr->operand) r->operand = rewriteExpr(expr->operand);
        if (expr->array_base) r->array_base = rewriteExpr(expr->array_base);
        if (expr->index) r->index = rewriteExpr(expr->index);
        if (expr->struct_base) r->struct_base = rewriteExpr(expr->struct_base);
        if (expr->cast_expr) r->cast_expr = rewriteExpr(expr->cast_expr);
        if (expr->cond) r->cond = rewriteExpr(expr->cond);
        if (expr->then_expr) r->then_expr = rewriteExpr(expr->then_expr);
        if (expr->else_expr) r->else_expr = rewriteExpr(expr->else_expr);
        if (expr->base) r->base = rewriteExpr(expr->base);
        if (expr->value) r->value = rewriteExpr(expr->value);
        r->args.clear();
        for (auto& arg : expr->args) r->args.push_back(rewriteExpr(arg));
        r->parts.clear();
        for (auto& part : expr->parts) r->parts.push_back(rewriteExpr(part));
        return error_.empty() ? r : nullptr;
    }

    std::vector<StmtPtr> inlineProcedureCall(const ExprPtr& call) {
        std::vector<StmtPtr> out;
        if (!call || call->kind != ExprKind::Call) return out;

        std::string callee = call->callee;
        const FunctionAST* helper = resolveCallee(callee, call->type, call->args.size());
        if (!helper || !error_.empty()) return out;

        std::vector<std::string> param_names;
        for (auto& p : helper->params) param_names.push_back(p.name);
        std::size_t arg_offset = call->args.size() == param_names.size() + 1 ? 1 : 0;
        if (param_names.size() != call->args.size() - arg_offset) {
            error_ = "Helper function '" + callee + "' argument count mismatch";
            return out;
        }

        int inline_id = inline_counter_++;
        std::unordered_map<std::string, ExprPtr> arg_map;
        std::vector<StmtPtr> value_param_decls;
        for (std::size_t i = 0; i < param_names.size(); ++i) {
            auto arg = rewriteExpr(call->args[i + arg_offset]);
            if (!arg || !error_.empty()) return out;
            const auto& param = helper->params[i];
            if (param.passing == ParamPassingKind::Value &&
                param.type.width > 0 &&
                !param.type.is_array &&
                arg->kind != ExprKind::VarRef &&
                arg->kind != ExprKind::Literal) {
                std::ostringstream name;
                name << param.name << "__arg_" << inline_id << "_" << i;
                auto temp = make_var(name.str(), param.type);
                auto decl = std::make_shared<Stmt>();
                decl->kind = StmtKind::Decl;
                decl->decl_name = name.str();
                decl->decl_type = param.type;
                decl->decl_init = castIfWidthChanges(arg, param.type);
                value_param_decls.push_back(std::move(decl));
                arg_map[param.name] = std::move(temp);
            } else {
                arg_map[param.name] = arg;
            }
        }
        collectLocalRenames(helper->body, param_names, inline_id, arg_map);
        out = substituteInlineStmts(helper->body, arg_map);
        out.insert(out.begin(), value_param_decls.begin(), value_param_decls.end());
        out = localizeProcedureReturns(out, callee, error_);
        if (!error_.empty()) return {};
        return rewriteStmts(out);
    }

    StmtPtr rewriteStmt(const StmtPtr& stmt) {
        if (!stmt || !error_.empty()) return nullptr;
        auto r = std::make_shared<Stmt>(*stmt);
        switch (stmt->kind) {
        case StmtKind::Assign:
            r->assign_target = rewriteExpr(stmt->assign_target);
            r->assign_value = rewriteExpr(stmt->assign_value);
            return r;
        case StmtKind::Decl:
            if (stmt->decl_init.has_value()) r->decl_init = rewriteExpr(stmt->decl_init.value());
            return r;
        case StmtKind::If:
            r->if_cond = rewriteExpr(stmt->if_cond);
            r->if_then = rewriteStmts(stmt->if_then);
            r->if_else = rewriteStmts(stmt->if_else);
            return r;
        case StmtKind::Block:
            r->block_stmts = rewriteStmts(stmt->block_stmts);
            return r;
        case StmtKind::For:
            if (stmt->for_init) r->for_init = rewriteStmt(stmt->for_init);
            r->for_cond = rewriteExpr(stmt->for_cond);
            r->for_step = rewriteExpr(stmt->for_step);
            r->for_body = rewriteStmts(stmt->for_body);
            return r;
        case StmtKind::While:
        case StmtKind::DoWhile:
            r->while_cond = rewriteExpr(stmt->while_cond);
            r->while_body = rewriteStmts(stmt->while_body);
            return r;
        case StmtKind::Switch:
            r->switch_expr = rewriteExpr(stmt->switch_expr);
            for (auto& clause : r->switch_cases) {
                if (clause.value.has_value()) clause.value = rewriteExpr(clause.value.value());
                clause.body = rewriteStmts(clause.body);
            }
            return r;
        case StmtKind::Return:
            if (stmt->return_value.has_value()) r->return_value = rewriteExpr(stmt->return_value.value());
            return r;
        case StmtKind::ExprStmt:
            r->expr_stmt = rewriteExpr(stmt->expr_stmt);
            return r;
        default:
            return r;
        }
    }

    std::vector<StmtPtr> rewriteStmts(const std::vector<StmtPtr>& stmts) {
        std::vector<StmtPtr> out;
        for (const auto& stmt : stmts) {
            if (!stmt || !error_.empty()) continue;
            if (stmt->kind == StmtKind::ExprStmt && isInlineCallee(stmt->expr_stmt)) {
                auto inlined = inlineProcedureCall(stmt->expr_stmt);
                out.insert(out.end(), inlined.begin(), inlined.end());
                continue;
            }
            auto rewritten = rewriteStmt(stmt);
            if (rewritten && error_.empty()) out.push_back(std::move(rewritten));
        }
        return out;
    }
};

} // namespace

InlineResult inlineHelpersAndLambdas(const FunctionAST& func,
                                     const std::vector<StmtPtr>& body) {
    InlinePass pass(func);
    return pass.run(body);
}

} // namespace pred
