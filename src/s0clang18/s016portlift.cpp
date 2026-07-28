#include "s0clang18/s016portlift.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pred::s0clang18 {
namespace {

using pred::v2::ExprPtr;
using pred::v2::FunctionAST;
using pred::v2::ParamDecl;
using pred::v2::StmtPtr;

std::string implicitPortParamName(const std::string& port_name) {
    return "__rtlzz_port_" + port_name;
}

Diagnostic makeError(DebugLoc loc, std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.16";
    diagnostic.context.loc = std::move(loc);
    diagnostic.context.source_file = diagnostic.context.loc.file;
    return diagnostic;
}

bool hasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

ParamDecl paramForPort(const RawPortDecl& port, bool implicit) {
    ParamDecl param;
    param.type = port.type;
    param.name = implicit ? implicitPortParamName(port.name) : port.name;
    param.debug_loc = port.decl_loc;
    param.direction = toV2Direction(port.direction);
    param.is_output = port.direction == PortDirection::Output;
    return param;
}

ImplicitPortParam implicitParamForPort(const RawPortDecl& port) {
    ImplicitPortParam implicit;
    implicit.port_name = port.name;
    implicit.param_name = implicitPortParamName(port.name);
    implicit.param = paramForPort(port, true);
    implicit.loc = port.decl_loc.valid() ? port.decl_loc : port.pragma_loc;
    return implicit;
}

const RawPortDecl* findPort(const PortDeclTable& ports, const std::string& name) {
    auto found = ports.port_by_name.find(name);
    if (found == ports.port_by_name.end()) return nullptr;
    if (found->second >= ports.ports.size()) return nullptr;
    return &ports.ports[found->second];
}

template <typename Fn>
void walkExpr(const ExprPtr& expr, Fn& fn) {
    if (!expr) return;
    fn(expr);
    for (const auto& child : {expr->left, expr->right, expr->operand,
                              expr->array_base, expr->index, expr->struct_base,
                              expr->cast_expr, expr->cond, expr->then_expr,
                              expr->else_expr, expr->base, expr->value}) {
        walkExpr(child, fn);
    }
    for (const auto& arg : expr->args) walkExpr(arg, fn);
    for (const auto& part : expr->parts) walkExpr(part, fn);
}

template <typename Fn>
void walkStmt(const StmtPtr& stmt, Fn& fn) {
    if (!stmt) return;
    for (const auto& expr : {stmt->assign_target, stmt->assign_value,
                             stmt->if_cond, stmt->for_cond, stmt->for_step,
                             stmt->while_cond, stmt->switch_expr, stmt->expr_stmt}) {
        walkExpr(expr, fn);
    }
    if (stmt->decl_init) walkExpr(*stmt->decl_init, fn);
    for (const auto& arg : stmt->decl_init_args) walkExpr(arg, fn);
    if (stmt->for_init) walkStmt(stmt->for_init, fn);
    for (const auto& child : stmt->if_then) walkStmt(child, fn);
    for (const auto& child : stmt->if_else) walkStmt(child, fn);
    for (const auto& child : stmt->for_body) walkStmt(child, fn);
    for (const auto& child : stmt->while_body) walkStmt(child, fn);
    for (const auto& child : stmt->block_stmts) walkStmt(child, fn);
    for (const auto& clause : stmt->switch_cases) {
        if (clause.value) walkExpr(*clause.value, fn);
        for (const auto& child : clause.body) walkStmt(child, fn);
    }
    if (stmt->return_value) walkExpr(*stmt->return_value, fn);
}

template <typename Fn>
void walkFunctionBody(const FunctionAST& function, Fn&& fn) {
    for (const auto& stmt : function.body) walkStmt(stmt, fn);
}

template <typename Fn>
void walkFunctionBodyMutable(FunctionAST& function, Fn&& fn) {
    for (const auto& stmt : function.body) walkStmt(stmt, fn);
}

void collectFunctions(FunctionAST& root, std::vector<FunctionAST*>& out) {
    out.push_back(&root);
    for (auto& helper : root.helpers) {
        if (helper) collectFunctions(*helper, out);
    }
    for (auto& [_, lambda] : root.lambdas) {
        if (lambda) collectFunctions(*lambda, out);
    }
}

void collectFunctionsConst(const FunctionAST& root,
                           std::vector<const FunctionAST*>& out) {
    out.push_back(&root);
    for (const auto& helper : root.helpers) {
        if (helper) collectFunctionsConst(*helper, out);
    }
    for (const auto& [_, lambda] : root.lambdas) {
        if (lambda) collectFunctionsConst(*lambda, out);
    }
}

std::unordered_map<std::string, const FunctionEntity*> entitiesByName(
    const FunctionReachabilityGraph& reachability) {
    std::unordered_map<std::string, const FunctionEntity*> out;
    for (const FunctionEntity& entity : reachability.functions) {
        out.emplace(entity.key.stable_name, &entity);
    }
    return out;
}

std::optional<std::string> referencedPortName(const pred::v2::Expr& expr,
                                              const PortDeclTable& ports) {
    if (!expr.global_port_name.empty() && findPort(ports, expr.global_port_name)) {
        return expr.global_port_name;
    }
    return std::nullopt;
}

const FunctionPortRequirement* requirementForId(const PortLiftPlan& plan,
                                                SemanticEntityId id) {
    auto found = plan.requirement_by_function.find(id);
    if (found == plan.requirement_by_function.end()) return nullptr;
    if (found->second >= plan.requirements.size()) return nullptr;
    return &plan.requirements[found->second];
}

FunctionPortRequirement* requirementForName(PortLiftPlan& plan,
                                            const std::string& name) {
    for (auto& requirement : plan.requirements) {
        if (requirement.function_name == name) return &requirement;
    }
    return nullptr;
}

const FunctionPortRequirement* requirementForName(const PortLiftPlan& plan,
                                                  const std::string& name) {
    for (const auto& requirement : plan.requirements) {
        if (requirement.function_name == name) return &requirement;
    }
    return nullptr;
}

bool containsPort(const FunctionPortRequirement& requirement,
                  const std::string& port_name) {
    return std::any_of(requirement.ports.begin(), requirement.ports.end(),
                       [&](const ImplicitPortParam& port) {
                           return port.port_name == port_name;
                       });
}

bool addPortRequirement(FunctionPortRequirement& requirement,
                        const RawPortDecl& port) {
    if (containsPort(requirement, port.name)) return false;
    requirement.ports.push_back(implicitParamForPort(port));
    return true;
}

void sortRequirementPorts(FunctionPortRequirement& requirement,
                          const PortDeclTable& ports) {
    std::unordered_map<std::string, std::size_t> order;
    for (std::size_t i = 0; i < ports.ports.size(); ++i) {
        order.emplace(ports.ports[i].name, i);
    }
    std::sort(requirement.ports.begin(), requirement.ports.end(),
              [&](const ImplicitPortParam& lhs, const ImplicitPortParam& rhs) {
                  return order[lhs.port_name] < order[rhs.port_name];
              });
}

bool hasParamNamed(const FunctionAST& function, const std::string& name) {
    return std::any_of(function.params.begin(), function.params.end(),
                       [&](const ParamDecl& param) {
                           return param.name == name;
                       });
}

bool hasCallArgForPort(const pred::v2::Expr& call,
                       const std::string& arg_name) {
    for (const auto& arg : call.args) {
        if (arg && arg->kind == pred::v2::ExprKind::VarRef &&
            arg->var_name == arg_name) {
            return true;
        }
    }
    return false;
}

std::unordered_map<std::string, FunctionAST*> functionsByName(
    const std::vector<FunctionAST*>& functions) {
    std::unordered_map<std::string, FunctionAST*> out;
    for (FunctionAST* function : functions) {
        if (function && !function->name.empty()) out.emplace(function->name, function);
    }
    return out;
}

} // namespace

StepResult<PortLiftPlan> analyzeGlobalPortRequirements(
    const pred::v2::FunctionAST& function,
    const PortDeclTable& ports,
    const FunctionReachabilityGraph& reachability) {
    StepResult<PortLiftPlan> result;
    PortLiftPlan plan;

    std::vector<const FunctionAST*> functions;
    collectFunctionsConst(function, functions);
    auto entities = entitiesByName(reachability);

    std::unordered_map<const FunctionAST*, SemanticEntityId> function_ids;
    for (const FunctionAST* ast_function : functions) {
        if (!ast_function) continue;
        auto entity = entities.find(ast_function->name);
        if (entity == entities.end()) continue;
        FunctionPortRequirement requirement;
        requirement.function_id = entity->second->id;
        requirement.function_name = ast_function->name;
        plan.requirement_by_function[requirement.function_id] =
            plan.requirements.size();
        plan.requirements.push_back(std::move(requirement));
        function_ids.emplace(ast_function, entity->second->id);
    }

    for (const FunctionAST* ast_function : functions) {
        if (!ast_function) continue;
        auto id_found = function_ids.find(ast_function);
        if (id_found == function_ids.end()) continue;
        FunctionPortRequirement* requirement =
            requirementForName(plan, ast_function->name);
        if (!requirement) continue;

        auto collect_direct = [&](const ExprPtr& expr) {
            if (!expr) return;
            std::optional<std::string> port_name = referencedPortName(*expr, ports);
            if (!port_name) return;
            const RawPortDecl* port = findPort(ports, *port_name);
            if (port) addPortRequirement(*requirement, *port);
        };
        walkFunctionBody(*ast_function, collect_direct);
    }

    const std::size_t max_iterations = reachability.functions.size() *
                                       reachability.functions.size() + 1;
    for (std::size_t iteration = 0; iteration < max_iterations; ++iteration) {
        bool changed = false;
        for (const FunctionCallEdge& edge : reachability.call_edges) {
            FunctionPortRequirement* caller =
                requirementForName(plan,
                                   findFunctionEntity(reachability, edge.caller)
                                       ? findFunctionEntity(reachability, edge.caller)
                                             ->key.stable_name
                                       : std::string{});
            const FunctionPortRequirement* callee =
                requirementForId(plan, edge.callee);
            if (!caller || !callee) continue;
            for (const ImplicitPortParam& port : callee->ports) {
                const RawPortDecl* raw = findPort(ports, port.port_name);
                if (raw) changed |= addPortRequirement(*caller, *raw);
            }
        }
        if (!changed) break;
        if (iteration + 1 == max_iterations) {
            result.diagnostics.push_back(makeError(
                {}, "S0Clang18 global port dependency propagation did not converge"));
            break;
        }
    }

    for (FunctionPortRequirement& requirement : plan.requirements) {
        sortRequirementPorts(requirement, ports);
    }

    if (hasError(result.diagnostics)) return result;
    result.value = std::move(plan);
    return result;
}

PortLiftResult liftGlobalPorts(pred::v2::FunctionAST function,
                               const PortDeclTable& ports,
                               const PortLiftPlan& plan) {
    PortLiftResult result;
    result.plan = plan;

    std::vector<FunctionAST*> functions;
    collectFunctions(function, functions);
    auto by_name = functionsByName(functions);

    for (FunctionAST* ast_function : functions) {
        if (!ast_function) continue;
        const bool is_top = ast_function == &function;
        if (is_top) {
            for (const RawPortDecl& port : ports.ports) {
                if (hasParamNamed(*ast_function, port.name)) continue;
                ast_function->params.push_back(paramForPort(port, false));
            }
        } else if (const FunctionPortRequirement* requirement =
                       requirementForName(plan, ast_function->name)) {
            for (const ImplicitPortParam& implicit : requirement->ports) {
                if (hasParamNamed(*ast_function, implicit.param_name)) continue;
                ast_function->params.push_back(implicit.param);
            }
        }

        auto rewrite_refs = [&](const ExprPtr& expr) {
            if (!expr) return;
            std::optional<std::string> port_name = referencedPortName(*expr, ports);
            if (!port_name) return;
            expr->kind = pred::v2::ExprKind::VarRef;
            expr->var_name = is_top ? *port_name : implicitPortParamName(*port_name);
            expr->global_port_name.clear();
        };
        walkFunctionBodyMutable(*ast_function, rewrite_refs);
    }

    for (FunctionAST* caller : functions) {
        if (!caller) continue;
        const bool caller_is_top = caller == &function;
        auto append_port_args = [&](const ExprPtr& expr) {
            if (!expr || expr->kind != pred::v2::ExprKind::Call) return;
            auto callee_ast = by_name.find(expr->callee);
            if (callee_ast == by_name.end()) return;
            const FunctionPortRequirement* callee_requirement =
                requirementForName(plan, callee_ast->second->name);
            if (!callee_requirement) return;
            for (const ImplicitPortParam& port : callee_requirement->ports) {
                std::string arg_name = caller_is_top
                    ? port.port_name
                    : implicitPortParamName(port.port_name);
                if (hasCallArgForPort(*expr, arg_name)) continue;
                ExprPtr arg = pred::v2::make_var(arg_name, port.param.type);
                arg->debug_loc = expr->debug_loc;
                expr->args.push_back(std::move(arg));
            }
        };
        walkFunctionBodyMutable(*caller, append_port_args);
    }

    result.function = std::move(function);
    return result;
}

} // namespace pred::s0clang18
