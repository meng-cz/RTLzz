#include "transform/Normalize.h"
#include "predicate/PredicateIR.h"
#include "semantics/AliasGraph.h"
#include "semantics/IntSemantics.h"
#include "normalize/NormalizeUtils.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace pred {

namespace {

static constexpr bool allowUnsafeTextFallback = false;

void addOutputParam(std::vector<std::string>& outputs, const std::string& name) {
    if (name.empty()) return;
    if (std::find(outputs.begin(), outputs.end(), name) == outputs.end()) {
        outputs.push_back(name);
    }
}

struct Env;
ExprPtr rewriteExpr(const ExprPtr& e, Env& env);
std::vector<StmtPtr> rewriteStmts(const std::vector<StmtPtr>& stmts, Env& env);

struct Env {
    std::unordered_map<std::string, TypeInfo> symbols;
    std::unordered_map<std::string, TypeInfo> ssa_seed_symbols;
    std::unordered_set<std::string> initialized;
    std::unordered_map<std::string, std::vector<bool>> bit_initialized;
    std::unordered_map<std::string, FunctionAST> helpers;
    std::unordered_map<std::string, FunctionAST> lambdas;
    std::unordered_map<std::string, std::vector<StructFieldInfo>> struct_fields;
    std::unordered_map<std::string, std::vector<StructConstructorInfo>> struct_constructors;
    std::unordered_map<std::string, std::vector<std::string>> lookup_tables;
    std::unordered_map<std::string, std::string> param_directions;
    std::unordered_map<std::string, std::string> output_default_reasons;
    std::unordered_map<std::string, std::string> output_paired_controls;
    struct RegProxyAlias {
        std::string rdata;
        std::string wen;
        std::string wdata;
    };
    struct ReqHelperAlias {
        std::string vld;
        std::string arg_data;
    };
    std::unordered_map<std::string, RegProxyAlias> regproxy_aliases;
    std::unordered_map<std::string, ReqHelperAlias> reqhelper_aliases;
    AliasGraph alias_graph;
    std::unordered_set<std::string> input_arrays;
    std::vector<std::string> output_params;
    int inline_counter = 0;
    std::string error;
};

void markFormalWrite(Env& env, const std::string& name) {
    if (name.empty()) return;
    addOutputParam(env.output_params, name);
    auto sym = env.symbols.find(name);
    if (sym != env.symbols.end()) {
        env.ssa_seed_symbols[name] = sym->second;
    }
    auto it = env.param_directions.find(name);
    if (it != env.param_directions.end()) {
        if (it->second == "Input") it->second = "Output";
    }
}

bool isFormalOutput(const Env& env, const std::string& name) {
    auto it = env.param_directions.find(name);
    return it != env.param_directions.end() &&
           (it->second == "Output" || it->second == "InOut");
}

ExprPtr zeroValueForType(const TypeInfo& type) {
    if (type.width == 1 || type.hw_kind == "bool" || type.name == "bool") {
        return make_literal("false", TypeInfo{"bool", 1, false, true, "bool"});
    }
    TypeInfo t = type;
    if (t.width <= 0) t = make_hw_type("UInt", 1, false);
    return make_literal("0", t);
}

void addSeedSymbol(Env& env, const std::string& name, const TypeInfo& type) {
    if (!name.empty()) env.ssa_seed_symbols[name] = type;
}

void addSeedSymbolsWithPrefix(Env& env, const std::string& prefix) {
    for (auto& [name, type] : env.symbols) {
        if (name == prefix || name.rfind(prefix + "_", 0) == 0) {
            env.ssa_seed_symbols[name] = type;
        }
    }
}

bool allFlattenedElementsInitialized(const std::string& name,
                                     const TypeInfo& arr_type,
                                     Env& env,
                                     std::vector<int>& prefix,
                                     size_t depth);
std::string firstUninitializedFlattenedElement(const std::string& name,
                                               const TypeInfo& arr_type,
                                               Env& env,
                                               std::vector<int>& prefix,
                                               size_t depth);

bool isRegProxyType(const TypeInfo& type) {
    return type.struct_name.find("__RegProxy") != std::string::npos ||
           type.name.find("__RegProxy") != std::string::npos;
}

bool isReqHelperType(const TypeInfo& type) {
    return type.struct_name.find("__ReqHelper") != std::string::npos ||
           type.name.find("__ReqHelper") != std::string::npos;
}

TypeInfo symbolType(Env& env, const std::string& name, TypeInfo fallback = {}) {
    auto it = env.symbols.find(name);
    return it == env.symbols.end() ? fallback : it->second;
}

ExprPtr readRegProxy(const std::string& proxy, TypeInfo desired_type, Env& env) {
    auto alias_it = env.regproxy_aliases.find(proxy);
    if (alias_it == env.regproxy_aliases.end()) return nullptr;
    const auto& alias = alias_it->second;
    TypeInfo rdata_type = symbolType(env, alias.rdata);
    if (rdata_type.width <= 0) {
        env.error = "Unsupported RegProxy rdata alias without known width for '" + proxy + "'";
        return nullptr;
    }
    int width = desired_type.width > 0 ? desired_type.width : rdata_type.width;
    int hi = std::min(width, rdata_type.width) - 1;
    if (hi < 0) {
        env.error = "Unsupported zero-width RegProxy read for '" + proxy + "'";
        return nullptr;
    }
    bool is_signed = desired_type.width > 0 ? desired_type.is_signed : rdata_type.is_signed;
    return make_slice(make_var(alias.rdata, rdata_type),
                      hi,
                      0,
                      make_hw_type(is_signed ? "Int" : "UInt", hi + 1, is_signed));
}

std::optional<Env::ReqHelperAlias> resolveReqHelperAlias(const ExprPtr& maybe_receiver, Env& env) {
    std::string receiver = baseName(maybe_receiver);
    if (!receiver.empty()) {
        auto it = env.reqhelper_aliases.find(receiver);
        if (it != env.reqhelper_aliases.end()) return it->second;
        env.error = "Unsupported output call with unknown ReqHelper alias '" + receiver + "'";
        return std::nullopt;
    }
    if (env.reqhelper_aliases.size() == 1) return env.reqhelper_aliases.begin()->second;
    if (env.reqhelper_aliases.empty()) {
        env.error = "Unsupported output call without ReqHelper constructor alias";
    } else {
        env.error = "Unsupported output call with ambiguous ReqHelper alias";
    }
    return std::nullopt;
}

bool isRegProxyWdataAliasTarget(const std::string& name, const Env& env) {
    for (const auto& item : env.regproxy_aliases) {
        const auto& wdata = item.second.wdata;
        if (name == wdata || name.rfind(wdata + "_", 0) == 0) return true;
    }
    return false;
}

std::string canonicalOutputNameFromReqHelperAlias(const std::string& name, const Env& env) {
    for (const auto& item : env.reqhelper_aliases) {
        const auto& alias = item.second;
        if (!alias.arg_data.empty() && name == alias.arg_data) return alias.arg_data;
    }
    return name;
}

void replaceOutputParamName(Env& env, const std::string& old_name, const std::string& new_name) {
    if (old_name == new_name) return;
    for (auto& name : env.output_params) {
        if (name == old_name) name = new_name;
    }
}

void markScalarFullyInitialized(Env& env, const std::string& name, const TypeInfo& type) {
    env.initialized.insert(name);
    if (type.width > 0) {
        env.bit_initialized[name] = std::vector<bool>(static_cast<size_t>(type.width), true);
    }
}

void markBitRangeInitialized(Env& env,
                             const std::string& name,
                             const TypeInfo& type,
                             int hi,
                             int lo) {
    if (type.width <= 0) {
        env.initialized.insert(name);
        return;
    }
    if (hi < lo) std::swap(hi, lo);
    if (hi < 0 || lo < 0 || hi >= type.width) {
        env.error = "Bit/slice assignment out of bounds for '" + name + "'";
        return;
    }
    auto& bits = env.bit_initialized[name];
    if (bits.size() != static_cast<size_t>(type.width)) {
        bits.assign(static_cast<size_t>(type.width), false);
    }
    for (int i = lo; i <= hi; ++i) bits[static_cast<size_t>(i)] = true;
    if (std::all_of(bits.begin(), bits.end(), [](bool v) { return v; })) {
        env.initialized.insert(name);
    }
}

bool hasAnyBitInitialized(Env& env, const std::string& name) {
    auto it = env.bit_initialized.find(name);
    if (it == env.bit_initialized.end()) return false;
    return std::any_of(it->second.begin(), it->second.end(), [](bool v) { return v; });
}

ExprPtr inlineHelperCall(const ExprPtr& e, Env& env) {
    if (e->callee == "__bit" || e->callee == "__slice") {
        std::vector<ExprPtr> args;
        for (auto& arg : e->args) {
            args.push_back(rewriteExpr(arg, env));
            if (!env.error.empty()) return nullptr;
        }
        if (args.empty()) {
            env.error = "Unsupported bit/slice base expression";
            return nullptr;
        }
        int base_width = args.front() ? args.front()->type.width : 0;
        if (e->callee == "__bit") {
            if (args.size() < 2) {
                env.error = "bit_select requires static bit index";
                return nullptr;
            }
            int bit = literalIntValue(args[1], -1);
            if (bit < 0 || (base_width > 0 && bit >= base_width)) {
                env.error = "Bit select out of bounds";
                return nullptr;
            }
            return make_bit_select(args.front(), bit);
        }
        if (args.size() < 3) {
            env.error = "slice requires static hi/lo";
            return nullptr;
        }
        int hi = literalIntValue(args[1], -1);
        int lo = literalIntValue(args[2], -1);
        if (hi < 0 || lo < 0 || hi < lo || (base_width > 0 && hi >= base_width)) {
            env.error = "Slice out of bounds";
            return nullptr;
        }
        TypeInfo ty = make_hw_type(args.front()->type.is_signed ? "Int" : "UInt", hi - lo + 1, args.front()->type.is_signed);
        return make_slice(args.front(), hi, lo, ty);
    }

    if (e->callee == "__cat") {
        std::vector<ExprPtr> parts;
        for (auto& arg : e->args) {
            auto rewritten = rewriteExpr(arg, env);
            if (!env.error.empty()) return nullptr;
            parts.push_back(rewritten);
        }
        return make_concat(std::move(parts));
    }

    if (e->callee == "__repeat") {
        int count = e->args.empty() ? 0 : literalIntValue(e->args.front(), 0);
        if (e->args.size() < 2) {
            env.error = "repeat requires a value argument";
            return nullptr;
        }
        auto value = rewriteExpr(e->args[1], env);
        if (!env.error.empty()) return nullptr;
        if (count <= 0) {
            env.error = "repeat count must be a positive compile-time constant";
            return nullptr;
        }
        return make_repeat(value, count);
    }

    if (e->callee == "__unsupported_expr") {
        env.error = "Unsupported expression at " + e->literal_value;
        return nullptr;
    }

    if (e->intrinsic == IntrinsicKind::DynamicRangeAt || e->callee == "__dynamic_range_at") {
        if (e->args.size() < 2) {
            env.error = "Unsupported dynamic range_at; requires receiver and index";
            return nullptr;
        }
        auto out = std::make_shared<Expr>(*e);
        out->args.clear();
        out->args.push_back(rewriteExpr(e->args[0], env));
        if (!env.error.empty()) return nullptr;
        out->args.push_back(rewriteExpr(e->args[1], env));
        if (!env.error.empty()) return nullptr;
        if (out->type.width <= 0) {
            env.error = "Unsupported dynamic range_at with unknown width";
            return nullptr;
        }
        return out;
    }
    if (e->intrinsic == IntrinsicKind::DynamicBitAt || e->callee == "__dynamic_bit_at") {
        if (e->args.size() < 2) {
            env.error = "Unsupported dynamic bit_at; requires receiver and index";
            return nullptr;
        }
        auto out = std::make_shared<Expr>(*e);
        out->args.clear();
        out->args.push_back(rewriteExpr(e->args[0], env));
        if (!env.error.empty()) return nullptr;
        out->args.push_back(rewriteExpr(e->args[1], env));
        if (!env.error.empty()) return nullptr;
        out->type = make_hw_type("bool", 1, false);
        return out;
    }

    if (e->callee == "lookup") {
        if (e->args.size() < 2) {
            env.error = "lookup requires table and index operands";
            return nullptr;
        }
        auto out = std::make_shared<Expr>(*e);
        out->args.clear();
        out->args.push_back(cloneExpr(e->args.front()));
        out->args.push_back(rewriteExpr(e->args[1], env));
        if (!env.error.empty()) return nullptr;
        if (out->type.width <= 0) {
            env.error = "lookup output width must be known";
            return nullptr;
        }
        return out;
    }

    if (e->callee == "get") {
        if (e->args.empty()) {
            env.error = "Unsupported RegProxy get call without receiver";
            return nullptr;
        }
        std::string proxy;
        if (e->args.front() && e->args.front()->kind == ExprKind::FieldAccess) {
            proxy = baseName(e->args.front()->struct_base);
        } else {
            proxy = baseName(e->args.front());
        }
        auto value = readRegProxy(proxy, e->type, env);
        if (!value && env.error.empty()) {
            env.error = "Unsupported RegProxy get call without constructor alias for '" + proxy + "'";
        }
        return value;
    }

    if (e->callee.rfind("operator", 0) == 0) {
        std::string op = e->callee.substr(std::string("operator").size());
        op.erase(std::remove_if(op.begin(), op.end(), [](unsigned char c) {
            return std::isspace(c);
        }), op.end());
        if (op == "/" || op == "%") {
            env.error = IntSemantics::binaryResultType(op, TypeInfo{}, TypeInfo{}).error;
            return nullptr;
        }
        if ((op == "+" || op == "-" || op == "*" || op == "&" || op == "|" ||
             op == "^" || op == "<<" || op == ">>" || op == "==" ||
             op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") &&
            e->args.size() >= 2) {
            auto lhs = rewriteExpr(e->args[e->args.size() - 2], env);
            if (!env.error.empty()) return nullptr;
            auto rhs = rewriteExpr(e->args[e->args.size() - 1], env);
            if (!env.error.empty()) return nullptr;
            normalizeConstantOperandsForBinary(op, lhs, rhs);
            if ((op == "&" || op == "|" || op == "^") &&
                lhs && rhs && lhs->type.width > 0 && rhs->type.width > 0 &&
                lhs->type.width != rhs->type.width) {
                if (isWidthCastableConstantExpr(lhs)) {
                    lhs = castIfWidthChanges(lhs, rhs->type);
                } else if (isWidthCastableConstantExpr(rhs)) {
                    rhs = castIfWidthChanges(rhs, lhs->type);
                } else {
                    env.error = IntSemantics::binaryResultType(op, lhs->type, rhs->type).error;
                    return nullptr;
                }
            }
            if (auto folded = foldConstantOvershift(op, lhs, rhs)) return folded;
            return make_binary(op, lhs, rhs, resultTypeForBinary(op, lhs->type, rhs->type));
        }
        if ((op == "!" || op == "~" || op == "-") && !e->args.empty()) {
            auto operand = rewriteExpr(e->args.back(), env);
            if (!env.error.empty()) return nullptr;
            TypeInfo ty = op == "!" ? make_hw_type("bool", 1, false) : operand->type;
            return make_unary(op, operand, ty);
        }
    }

    const FunctionAST* helper_ptr = nullptr;
    auto helper_it = env.helpers.find(e->callee);
    if (helper_it != env.helpers.end()) helper_ptr = &helper_it->second;
    auto lambda_it = env.lambdas.find(e->callee);
    if (!helper_ptr && lambda_it != env.lambdas.end()) helper_ptr = &lambda_it->second;
    if (!helper_ptr) {
        env.error = "Unsupported function call '" + e->callee + "'";
        return nullptr;
    }

    const auto& helper = *helper_ptr;
    std::vector<std::string> param_names;
    for (auto& p : helper.params) param_names.push_back(p.name);
    if (param_names.empty() && helper.body.size() == 1 &&
        helper.body.front()->return_value.has_value()) {
        collectVarRefs(helper.body.front()->return_value.value(), param_names);
    }

    size_t arg_offset = 0;
    if (e->args.size() == param_names.size() + 1) {
        arg_offset = 1;
    }

    if (param_names.size() != e->args.size() - arg_offset) {
        env.error = "Helper function '" + e->callee + "' argument count mismatch: params=" +
            std::to_string(param_names.size()) + " args=" + std::to_string(e->args.size()) +
            " offset=" + std::to_string(arg_offset);
        return nullptr;
    }
    std::unordered_map<std::string, ExprPtr> arg_map;
    std::vector<std::string> used_refs;
    for (auto& stmt : helper.body) collectStmtVarRefs(stmt, used_refs);
    for (size_t i = 0; i < param_names.size(); ++i) {
        auto raw_arg = e->args[i + arg_offset];
        if (std::find(used_refs.begin(), used_refs.end(), param_names[i]) == used_refs.end()) {
            arg_map[param_names[i]] = cloneExpr(raw_arg);
            continue;
        }
        if (raw_arg && raw_arg->kind == ExprKind::VarRef) {
            TypeInfo arg_type = raw_arg->type;
            auto sym = env.symbols.find(raw_arg->var_name);
            if (sym != env.symbols.end()) arg_type = sym->second;
            if (arg_type.is_array && arg_type.array_dims.empty() && arg_type.array_size > 0) {
                arg_type.array_dims = {arg_type.array_size};
            }
            if (arg_type.is_array && !arg_type.array_dims.empty()) {
                std::vector<int> prefix;
                if (!allFlattenedElementsInitialized(raw_arg->var_name, arg_type, env, prefix, 0)) {
                    env.error = "Read of uninitialized array '" + raw_arg->var_name + "'";
                    return nullptr;
                }
                arg_map[param_names[i]] = cloneExpr(raw_arg);
                continue;
            }
        }
        auto arg = rewriteExpr(raw_arg, env);
        if (!env.error.empty()) return nullptr;
        arg_map[param_names[i]] = arg;
    }

    ExprPtr return_expr;
    for (auto& stmt : helper.body) {
        if (!stmt) continue;
        if (stmt->kind == StmtKind::Decl && stmt->decl_init.has_value()) {
            auto init = substituteInlineExpr(stmt->decl_init.value(), arg_map);
            auto rewritten_init = rewriteExpr(init, env);
            if (!env.error.empty()) return nullptr;
            arg_map[stmt->decl_name] = rewritten_init;
            continue;
        }
        if (stmt->kind == StmtKind::Return && stmt->return_value.has_value()) {
            return_expr = stmt->return_value.value();
            continue;
        }
        env.error = "Helper function '" + e->callee +
            "' used as expression must contain only local initialized declarations and return";
        return nullptr;
    }
    if (!return_expr) {
        env.error = "Helper function '" + e->callee + "' used as expression has no return value";
        return nullptr;
    }

    auto inlined = substituteInlineExpr(return_expr, arg_map);
    return rewriteExpr(inlined, env);
}

std::vector<StmtPtr> inlineProcedureCall(const ExprPtr& call, Env& env) {
    std::vector<StmtPtr> out;
    if (!call || call->kind != ExprKind::Call) return out;

    if (call->intrinsic == IntrinsicKind::RegProxySetNext || call->callee == "__vul_setnext") {
        if (call->args.size() < 3) {
            env.error = "Unsupported RegProxy setnext call";
            return out;
        }
        ExprPtr receiver = call->args[0];
        std::string proxy;
        if (receiver && receiver->kind == ExprKind::FieldAccess) {
            proxy = baseName(receiver->struct_base);
        } else {
            proxy = baseName(receiver);
        }
        int port = literalIntValue(call->args[1], -1);
        auto alias_it = env.regproxy_aliases.find(proxy);
        if (alias_it == env.regproxy_aliases.end()) {
            env.error = "Unsupported RegProxy setnext without constructor alias for '" + proxy + "'";
            return out;
        }
        std::string wdata = alias_it->second.wdata;
        std::string wen = alias_it->second.wen;
        TypeInfo wdata_type = env.symbols.count(wdata) ? env.symbols[wdata] : TypeInfo{};
        TypeInfo wen_type = env.symbols.count(wen) ? env.symbols[wen] : TypeInfo{"bool", 1, false};
        if (wdata_type.is_array && wdata_type.array_dims.empty() && wdata_type.array_size > 0) {
            wdata_type.array_dims = {wdata_type.array_size};
        }
        if (wen_type.is_array && wen_type.array_dims.empty() && wen_type.array_size > 0) {
            wen_type.array_dims = {wen_type.array_size};
        }
        if (port < 0) {
            if (wdata_type.is_array || wen_type.is_array) {
                env.error = "Unsupported non-constant RegProxy port index";
                return out;
            }
            port = 0;
        }
        TypeInfo wdata_scalar = wdata_type.is_array ? scalarTypeFromArray(wdata_type) : wdata_type;
        TypeInfo wen_scalar = wen_type.is_array ? scalarTypeFromArray(wen_type) : TypeInfo{"bool", 1, false};
        auto port_lit = make_literal(std::to_string(port), TypeInfo{"int", 32, true});
        ExprPtr wtarget = wdata_type.is_array
            ? make_array_access(make_var(wdata, wdata_type), cloneExpr(port_lit), wdata_scalar)
            : make_var(wdata, wdata_scalar);
        ExprPtr wentarget = wen_type.is_array
            ? make_array_access(make_var(wen, wen_type), cloneExpr(port_lit), wen_scalar)
            : make_var(wen, wen_scalar);
        out.push_back(makeAssignStmt(wtarget, cloneExpr(call->args[2])));
        out.push_back(makeAssignStmt(wentarget, make_literal("true", TypeInfo{"bool", 1, false})));
        return out;
    }

    if (call->intrinsic == IntrinsicKind::ReqHelperOutput || call->callee == "__vul_output") {
        if (call->args.empty()) {
            env.error = "Unsupported output call without data argument";
            return out;
        }
        ExprPtr receiver;
        ExprPtr value;
        if (call->args.size() >= 2 && call->args.front() &&
            call->args.front()->kind == ExprKind::VarRef &&
            env.reqhelper_aliases.count(call->args.front()->var_name)) {
            receiver = call->args.front();
            value = call->args[1];
        } else {
            value = call->args.back();
        }
        auto alias = resolveReqHelperAlias(receiver, env);
        if (!alias.has_value()) {
            env.error = "Unsupported output call without ReqHelper alias";
            return out;
        }
        TypeInfo valid_type = symbolType(env, alias->vld, TypeInfo{"bool", 1, false});
        out.push_back(makeAssignStmt(make_var(alias->vld, valid_type),
                                     make_literal("true", TypeInfo{"bool", 1, false})));
        TypeInfo data_type = symbolType(env, alias->arg_data, make_hw_type("Int", 128, false));
        TypeInfo elem_type = make_hw_type("UInt", 8, false);
        TypeInfo value_type = value ? value->type : TypeInfo{};
        if (value && value->kind == ExprKind::VarRef && env.symbols.count(value->var_name)) {
            value_type = env.symbols[value->var_name];
        }
        if (value_type.is_array) {
            elem_type = scalarTypeFromArray(value_type);
        } else if (value_type.width > 0) {
            elem_type = value_type;
        }
        int elem_width = std::max(1, elem_type.width);
        int count = data_type.width > 0 ? data_type.width / elem_width : 1;
        if (count <= 0 || data_type.width % elem_width != 0) {
            env.error = "Unsupported output pack width mismatch";
            return out;
        }
        for (int i = 0; i < count; ++i) {
            ExprPtr elem;
            if (value && value->kind == ExprKind::VarRef && value_type.is_array) {
                elem = make_array_access(cloneExpr(value),
                                         make_literal(std::to_string(i), TypeInfo{"int", 32, true}),
                                         elem_type);
            } else {
                elem = make_slice(cloneExpr(value), i * elem_width + elem_width - 1, i * elem_width, elem_type);
            }
            auto write = make_write_slice(make_var(alias->arg_data, data_type),
                                          i * elem_width + elem_width - 1,
                                          i * elem_width,
                                          elem,
                                          data_type);
            out.push_back(makeAssignStmt(make_var(alias->arg_data, data_type), write));
        }
        return out;
    }

    const FunctionAST* helper_ptr = nullptr;
    auto helper_it = env.helpers.find(call->callee);
    if (helper_it != env.helpers.end()) helper_ptr = &helper_it->second;
    auto lambda_it = env.lambdas.find(call->callee);
    if (!helper_ptr && lambda_it != env.lambdas.end()) helper_ptr = &lambda_it->second;
    if (!helper_ptr) {
        env.error = "Unsupported function call '" + call->callee + "'";
        return out;
    }

    const auto& helper = *helper_ptr;
    std::vector<std::string> param_names;
    for (auto& p : helper.params) param_names.push_back(p.name);
    size_t arg_offset = call->args.size() == param_names.size() + 1 ? 1 : 0;
    if (param_names.size() != call->args.size() - arg_offset) {
        env.error = "Helper function '" + call->callee + "' argument count mismatch";
        return out;
    }

    std::unordered_map<std::string, ExprPtr> arg_map;
    for (size_t i = 0; i < param_names.size(); ++i) {
        arg_map[param_names[i]] = cloneExpr(call->args[i + arg_offset]);
    }
    int inline_id = env.inline_counter++;
    for (auto& s : helper.body) {
        if (s && s->kind == StmtKind::Decl &&
            !s->decl_type.is_static &&
            s->decl_type.init_values.empty() &&
            std::find(param_names.begin(), param_names.end(), s->decl_name) == param_names.end()) {
            arg_map[s->decl_name] = make_var(s->decl_name + "__inl_" + std::to_string(inline_id), s->decl_type);
        }
    }
    out = substituteInlineStmts(helper.body, arg_map);
    out = localizeProcedureReturns(out, call->callee, env.error);
    return out;
}

ExprPtr buildArrayMux(const std::string& name,
                      const std::vector<int>& dims,
                      const std::vector<ExprPtr>& indices,
                      size_t depth,
                      std::vector<int>& prefix,
                      TypeInfo scalar_type) {
    if (depth >= indices.size() || depth >= dims.size()) {
        return make_var(joinIndexName(name, prefix), scalar_type);
    }

    int dim = dims[depth];
    prefix.push_back(dim - 1);
    ExprPtr result = buildArrayMux(name, dims, indices, depth + 1, prefix, scalar_type);
    prefix.pop_back();

    for (int i = dim - 2; i >= 0; --i) {
        prefix.push_back(i);
        auto value = buildArrayMux(name, dims, indices, depth + 1, prefix, scalar_type);
        prefix.pop_back();
        auto cond = make_binary("==",
            cloneExpr(indices[depth]),
            make_literal(std::to_string(i), indices[depth] ? indices[depth]->type : TypeInfo{"int", 32, true}),
            TypeInfo{"bool", 1, false});
        result = make_ite(cond, value, result, scalar_type);
    }

    return result;
}

void collectFlattenedArrayVars(const std::string& name,
                               const TypeInfo& arr_type,
                               std::vector<int>& prefix,
                               size_t depth,
                               std::vector<ExprPtr>& args,
                               int& width,
                               bool& is_signed) {
    if (depth >= arr_type.array_dims.size()) {
        TypeInfo scalar = scalarTypeFromArray(arr_type);
        args.push_back(make_var(joinIndexName(name, prefix), scalar));
        width += scalar.width;
        is_signed = is_signed || scalar.is_signed;
        return;
    }
    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        collectFlattenedArrayVars(name, arr_type, prefix, depth + 1, args, width, is_signed);
        prefix.pop_back();
    }
}

ExprPtr buildPackedArrayValue(const std::string& name, TypeInfo arr_type, Env& env) {
    if (arr_type.array_dims.empty() && arr_type.array_size > 0) {
        arr_type.array_dims = {arr_type.array_size};
    }
    std::vector<ExprPtr> args;
    int width = 0;
    bool is_signed = false;
    std::vector<int> prefix;
    prefix.clear();
    collectFlattenedArrayVars(name, arr_type, prefix, 0, args, width, is_signed);
    std::reverse(args.begin(), args.end());

    return make_concat(std::move(args));
}

ExprPtr parseOpaqueArrayVar(const ExprPtr& e) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    if (!e || e->kind != ExprKind::VarRef) return nullptr;
    std::string text = e->var_name;
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c);
    }), text.end());
    auto b = text.find('[');
    if (b == std::string::npos) return nullptr;
    std::string base = text.substr(0, b);
    if (base.empty()) return nullptr;
    ExprPtr out = make_var(base);
    size_t pos = b;
    while (pos < text.size()) {
        auto l = text.find('[', pos);
        if (l == std::string::npos) break;
        auto r = text.find(']', l);
        if (r == std::string::npos) return nullptr;
        std::string idx_text = text.substr(l + 1, r - l - 1);
        ExprPtr idx;
        if (!idx_text.empty() && std::all_of(idx_text.begin(), idx_text.end(), [](unsigned char c) {
                return std::isdigit(c);
            })) {
            idx = make_literal(idx_text, TypeInfo{"int", 32, true});
        } else {
            idx = make_var(idx_text, TypeInfo{"int", 32, true});
        }
        out = make_array_access(out, idx, e->type);
        pos = r + 1;
    }
    return out;
}

ExprPtr parseOpaqueHwMethodVar(const ExprPtr& e, Env& env) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    if (!e || e->kind != ExprKind::VarRef) return nullptr;
    (void)env;
    return nullptr;
}

ExprPtr buildPackedFlattenedPrefix(const std::string& name, Env& env) {
    std::vector<ExprPtr> args;
    int width = 0;
    bool is_signed = false;
    for (int i = 0;; ++i) {
        std::string flat = name + "_" + std::to_string(i);
        auto it = env.symbols.find(flat);
        if (it == env.symbols.end()) break;
        args.push_back(make_var(flat, it->second));
        width += it->second.width;
        is_signed = is_signed || it->second.is_signed;
    }
    if (args.empty()) return nullptr;
    std::reverse(args.begin(), args.end());
    return make_concat(std::move(args));
}

void addFlattenedArraySymbols(Env& env,
                              const std::string& name,
                              const TypeInfo& type,
                              bool initialized,
                              std::vector<int>& prefix,
                              size_t depth) {
    if (depth >= type.array_dims.size()) {
        auto flat = joinIndexName(name, prefix);
        env.symbols[flat] = scalarTypeFromArray(type);
        if (initialized) env.initialized.insert(flat);
        return;
    }
    for (int i = 0; i < type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        addFlattenedArraySymbols(env, name, type, initialized, prefix, depth + 1);
        prefix.pop_back();
    }
}

bool allFlattenedElementsInitialized(const std::string& name,
                                     const TypeInfo& arr_type,
                                     Env& env,
                                     std::vector<int>& prefix,
                                     size_t depth) {
    if (depth >= arr_type.array_dims.size()) {
        return env.initialized.count(joinIndexName(name, prefix)) > 0;
    }
    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        bool ok = allFlattenedElementsInitialized(name, arr_type, env, prefix, depth + 1);
        prefix.pop_back();
        if (!ok) return false;
    }
    return true;
}

std::string firstUninitializedFlattenedElement(const std::string& name,
                                               const TypeInfo& arr_type,
                                               Env& env,
                                               std::vector<int>& prefix,
                                               size_t depth) {
    if (depth >= arr_type.array_dims.size()) {
        std::string flat = joinIndexName(name, prefix);
        return env.initialized.count(flat) ? "" : flat;
    }
    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        std::string missing = firstUninitializedFlattenedElement(name, arr_type, env, prefix, depth + 1);
        prefix.pop_back();
        if (!missing.empty()) return missing;
    }
    return "";
}

bool validateLiteralBounds(const std::string& name,
                           const std::vector<ExprPtr>& indices,
                           const std::vector<int>& dims,
                           Env& env) {
    for (size_t i = 0; i < indices.size() && i < dims.size(); ++i) {
        auto lit = literalIndex(indices[i]);
        if (!lit.has_value()) continue;
        if (*lit < 0 || *lit >= dims[i]) {
            env.error = "Array index out of bounds for '" + name + "' at dimension " +
                std::to_string(i) + ": " + std::to_string(*lit) +
                " not in [0, " + std::to_string(dims[i]) + ")";
            return false;
        }
    }
    return true;
}

const std::vector<StructFieldInfo>* findStructFields(const TypeInfo& type, Env& env) {
    if (type.struct_name.empty()) return nullptr;
    auto direct = env.struct_fields.find(type.struct_name);
    if (direct != env.struct_fields.end()) return &direct->second;
    auto canonical = canonicalStructName(type.struct_name);
    auto it = env.struct_fields.find(canonical);
    if (it != env.struct_fields.end()) return &it->second;
    auto struct_it = env.struct_fields.find("struct " + canonical);
    if (struct_it != env.struct_fields.end()) return &struct_it->second;
    return nullptr;
}

const std::vector<StructConstructorInfo>* findStructConstructors(const TypeInfo& type, Env& env) {
    if (!type.struct_name.empty()) {
        auto direct = env.struct_constructors.find(type.struct_name);
        if (direct != env.struct_constructors.end()) return &direct->second;
    }
    std::string canonical = canonicalStructName(type.struct_name.empty() ? type.name : type.struct_name);
    auto it = env.struct_constructors.find(canonical);
    if (it != env.struct_constructors.end()) return &it->second;
    auto struct_it = env.struct_constructors.find("struct " + canonical);
    if (struct_it != env.struct_constructors.end()) return &struct_it->second;
    return nullptr;
}

bool checkStructOutputInitialized(const std::string& base,
                                  const TypeInfo& type,
                                  Env& env,
                                  std::string& missing) {
    auto fields = findStructFields(type, env);
    if (fields) {
        for (auto& field : *fields) {
            if (!checkStructOutputInitialized(base + "_" + field.name, field.type, env, missing)) {
                return false;
            }
        }
        return true;
    }

    TypeInfo t = type;
    if (t.is_array && t.array_dims.empty() && t.array_size > 0) t.array_dims = {t.array_size};
    if (t.is_array && !t.array_dims.empty()) {
        std::vector<int> prefix;
        missing = firstUninitializedFlattenedElement(base, t, env, prefix, 0);
        return missing.empty();
    }

    if (!env.initialized.count(base)) {
        missing = base;
        return false;
    }
    return true;
}

void addStructFieldSymbols(const std::string& base,
                           const TypeInfo& type,
                           Env& env,
                           bool initialized) {
    auto fields = findStructFields(type, env);
    if (!fields) {
        return;
    }
    for (auto& field : *fields) {
        std::string field_name = base + "_" + field.name;
        TypeInfo field_type = field.type;
        if (findStructFields(field_type, env)) {
            addStructFieldSymbols(field_name, field_type, env, initialized);
            continue;
        }
        if (field_type.is_array && field_type.array_dims.empty() && field_type.array_size > 0) {
            field_type.array_dims = {field_type.array_size};
        }
        env.symbols[field_name] = field_type;
        if (field_type.is_array && !field_type.array_dims.empty()) {
            std::vector<int> prefix;
            addFlattenedArraySymbols(env, field_name, field_type, initialized, prefix, 0);
        } else if (initialized) {
            env.initialized.insert(field_name);
        }
    }
}

void expandArrayInitRec(const std::string& name,
                        const TypeInfo& arr_type,
                        Env& env,
                        std::vector<StmtPtr>& out,
                        std::vector<int>& prefix,
                        int& flat_index,
                        size_t depth) {
    if (depth >= arr_type.array_dims.size()) {
        TypeInfo scalar = scalarTypeFromArray(arr_type);
        std::string value = "0";
        if (flat_index < static_cast<int>(arr_type.init_values.size())) {
            value = arr_type.init_values[flat_index];
        }
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::Assign;
        stmt->assign_target = make_var(joinIndexName(name, prefix), scalar);
        stmt->assign_value = make_literal(value, scalar);
        out.push_back(stmt);
        env.symbols[joinIndexName(name, prefix)] = scalar;
        env.initialized.insert(joinIndexName(name, prefix));
        ++flat_index;
        return;
    }
    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        expandArrayInitRec(name, arr_type, env, out, prefix, flat_index, depth + 1);
        prefix.pop_back();
    }
}

std::vector<StmtPtr> rewriteArrayInitDecl(const StmtPtr& s, Env& env) {
    std::vector<StmtPtr> out;
    if (!s || s->kind != StmtKind::Decl || !s->decl_type.is_array ||
        s->decl_type.init_values.empty()) {
        return out;
    }
    TypeInfo arr_type = s->decl_type;
    if (arr_type.array_dims.empty() && arr_type.array_size > 0) arr_type.array_dims = {arr_type.array_size};
    if (arr_type.array_dims.empty()) return out;
    if (arr_type.is_static) return out;

    env.symbols[s->decl_name] = arr_type;
    std::vector<int> prefix;
    addFlattenedArraySymbols(env, s->decl_name, arr_type, false, prefix, 0);
    prefix.clear();
    int flat_index = 0;
    expandArrayInitRec(s->decl_name, arr_type, env, out, prefix, flat_index, 0);
    return out;
}

void expandPackedArrayInitRec(const std::string& name,
                              const TypeInfo& arr_type,
                              const ExprPtr& packed,
                              Env& env,
                              std::vector<StmtPtr>& out,
                              std::vector<int>& prefix,
                              int& bit_offset,
                              size_t depth) {
    if (depth >= arr_type.array_dims.size()) {
        TypeInfo scalar = scalarTypeFromArray(arr_type);
        int width = std::max(1, scalar.width);
        if (packed && packed->type.width > 0 && bit_offset + width - 1 >= packed->type.width) {
            env.error = "Slice out of bounds while unpacking array '" + name + "'";
            return;
        }
        auto slice = make_slice(cloneExpr(packed), bit_offset + width - 1, bit_offset, scalar);

        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::Assign;
        auto flat = joinIndexName(name, prefix);
        stmt->assign_target = make_var(flat, scalar);
        stmt->assign_value = slice;
        out.push_back(stmt);
        env.symbols[flat] = scalar;
        env.initialized.insert(flat);
        bit_offset += width;
        return;
    }
    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        expandPackedArrayInitRec(name, arr_type, packed, env, out, prefix, bit_offset, depth + 1);
        prefix.pop_back();
    }
}

std::vector<StmtPtr> rewritePackedArrayInitDecl(const StmtPtr& s, Env& env) {
    std::vector<StmtPtr> out;
    if (!s || s->kind != StmtKind::Decl || !s->decl_type.is_array ||
        !s->decl_init.has_value()) {
        return out;
    }
    TypeInfo arr_type = s->decl_type;
    if (arr_type.array_dims.empty() && arr_type.array_size > 0) arr_type.array_dims = {arr_type.array_size};
    if (arr_type.array_dims.empty()) return out;

    env.symbols[s->decl_name] = arr_type;
    std::vector<int> prefix;
    addFlattenedArraySymbols(env, s->decl_name, arr_type, false, prefix, 0);

    ExprPtr packed;
    auto init = s->decl_init.value();
    std::string init_name = directVarName(init);
    if (!init_name.empty() && env.symbols.count(init_name)) {
        TypeInfo src_type = env.symbols[init_name];
        if (src_type.is_array && src_type.array_dims.empty() && src_type.array_size > 0) {
            src_type.array_dims = {src_type.array_size};
        }
        if (src_type.is_array && src_type.array_dims == arr_type.array_dims) {
            std::vector<int> missing_prefix;
            std::string missing = firstUninitializedFlattenedElement(init_name, src_type, env, missing_prefix, 0);
            if (!missing.empty()) {
                env.error = "Array declaration initializer from '" + init_name +
                    "' reads uninitialized element '" + missing + "'";
                return out;
            }
            TypeInfo scalar = scalarTypeFromArray(arr_type);
            std::vector<int> copy_prefix;
            std::function<void(size_t)> copy_rec = [&](size_t depth) {
                if (depth >= arr_type.array_dims.size()) {
                    auto stmt = std::make_shared<Stmt>();
                    stmt->kind = StmtKind::Assign;
                    auto dst = joinIndexName(s->decl_name, copy_prefix);
                    auto src = joinIndexName(init_name, copy_prefix);
                    stmt->assign_target = make_var(dst, scalar);
                    stmt->assign_value = make_var(src, scalar);
                    out.push_back(stmt);
                    env.symbols[dst] = scalar;
                    env.initialized.insert(dst);
                    return;
                }
                for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
                    copy_prefix.push_back(i);
                    copy_rec(depth + 1);
                    copy_prefix.pop_back();
                }
            };
            copy_rec(0);
            return out;
        }
    }
    if (!init_name.empty()) {
        auto alias_it = env.regproxy_aliases.find(init_name);
        if (alias_it != env.regproxy_aliases.end()) {
            std::string rdata = alias_it->second.rdata;
            if (env.symbols.count(rdata)) {
                packed = make_var(rdata, env.symbols[rdata]);
            }
        }
    }
    if (!packed) {
        packed = rewriteExpr(init, env);
        if (!env.error.empty()) return out;
    }

    int elem_count = flatElementCount(arr_type);
    TypeInfo scalar = scalarTypeFromArray(arr_type);
    if (packed && packed->type.width > 0 && elem_count > 1 &&
        scalar.width > 0 && scalar.width * elem_count > packed->type.width &&
        arr_type.width > 0 && arr_type.width % elem_count == 0) {
        arr_type.width = arr_type.width / elem_count;
    }

    int bit_offset = 0;
    prefix.clear();
    expandPackedArrayInitRec(s->decl_name, arr_type, packed, env, out, prefix, bit_offset, 0);
    return out;
}

ExprPtr rewriteArrayAccess(const ExprPtr& e, Env& env) {
    ExprPtr base;
    std::vector<ExprPtr> raw_indices;
    if (!collectArrayAccess(e, base, raw_indices) || !base) return cloneExpr(e);

    std::string name = baseName(base);
    if (name.empty()) {
        env.error = "Unsupported array base expression kind=" +
            std::to_string(base ? static_cast<int>(base->kind) : -1);
        return nullptr;
    }
    if (!env.initialized.count(name) && env.symbols.count(name) && !base->type.is_array &&
        !buildPackedFlattenedPrefix(name, env)) {
        env.error = "Read of uninitialized variable '" + name + "'";
        return nullptr;
    }

    std::vector<ExprPtr> indices;
    for (auto& idx : raw_indices) {
        indices.push_back(rewriteExpr(idx, env));
        if (!env.error.empty()) return nullptr;
    }

    TypeInfo arr_type = base->type;
    if (env.symbols.count(name)) arr_type = env.symbols[name];
    if (arr_type.is_array && arr_type.array_dims.empty() && arr_type.array_size > 0) {
        arr_type.array_dims = {arr_type.array_size};
    }
    if (!arr_type.is_array || arr_type.array_dims.empty()) {
        auto rb = rewriteExpr(base, env);
        if (!env.error.empty()) return nullptr;
        auto out = std::make_shared<Expr>();
        out->kind = ExprKind::ArrayAccess;
        out->array_base = rb;
        out->index = indices.empty() ? nullptr : indices.front();
        out->type = e->type;
        return out;
    }

    if (indices.size() > arr_type.array_dims.size()) {
        env.error = "Too many indices for array '" + name + "'";
        return nullptr;
    }
    if (!validateLiteralBounds(name, indices, arr_type.array_dims, env)) {
        return nullptr;
    }

    bool dynamic_single_index = indices.size() == 1 && !literalIndex(indices.front()).has_value();
    if (!arr_type.init_values.empty() && indices.size() == 1 &&
        (arr_type.is_static || dynamic_single_index)) {
        env.lookup_tables[name] = arr_type.init_values;
        return makeLookupExpr(name, arr_type, indices.front());
    }

    bool all_literal = true;
    std::vector<int> literal_prefix;
    for (auto& idx : indices) {
        auto lit = literalIndex(idx);
        if (!lit.has_value()) {
            all_literal = false;
            break;
        }
        literal_prefix.push_back(*lit);
    }
    if (all_literal) {
        auto flat = joinIndexName(name, literal_prefix);
        if (!env.initialized.count(flat) &&
            (env.input_arrays.count(name) || env.initialized.count(name))) {
            std::vector<int> prefix;
            addFlattenedArraySymbols(env, name, arr_type, true, prefix, 0);
        }
        if (!env.initialized.count(flat)) {
            env.error = "Read of uninitialized array element '" + flat + "'";
            return nullptr;
        }
        return make_var(flat, scalarTypeFromArray(arr_type));
    }

    std::vector<int> init_prefix;
    if (env.input_arrays.count(name) || env.initialized.count(name)) {
        addFlattenedArraySymbols(env, name, arr_type, true, init_prefix, 0);
        init_prefix.clear();
    }
    std::string missing = firstUninitializedFlattenedElement(name, arr_type, env, init_prefix, 0);
    if (!missing.empty()) {
        env.error = "Dynamic array read from '" + name +
            "' requires all elements to be initialized; missing element '" + missing + "'";
        return nullptr;
    }

    std::vector<int> prefix;
    TypeInfo scalar = scalarTypeFromArray(arr_type);
    return buildArrayMux(name, arr_type.array_dims, indices, 0, prefix, scalar);
}

ExprPtr rewriteExpr(const ExprPtr& e, Env& env) {
    if (!e || !env.error.empty()) return nullptr;

    switch (e->kind) {
    case ExprKind::Literal:
        return cloneExpr(e);
    case ExprKind::VarRef: {
        ExprPtr var = cloneExpr(e);
        auto sym_it = env.symbols.find(e->var_name);
        if (sym_it != env.symbols.end()) var->type = sym_it->second;
        if (e->type.hw_kind == "signed_view") {
            var->type.is_signed = true;
            var->type.hw_kind = "signed_view";
        }
        if (env.regproxy_aliases.count(e->var_name)) {
            return readRegProxy(e->var_name, var->type, env);
        }
        if (allowUnsafeTextFallback) {
            if (auto hw_method = parseOpaqueHwMethodVar(e, env)) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
                return hw_method;
            }
            if (auto array_access = parseOpaqueArrayVar(e)) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
                return rewriteArrayAccess(array_access, env);
            }
        }
        if (!env.symbols.count(e->var_name)) {
            env.error = "Unknown variable '" + e->var_name + "'";
            return nullptr;
        }
        auto sym = env.symbols.find(e->var_name);
        if (sym != env.symbols.end()) {
            TypeInfo t = sym->second;
            if (t.is_array && t.array_dims.empty() && t.array_size > 0) t.array_dims = {t.array_size};
            if (t.is_array && !t.array_dims.empty()) {
                return buildPackedArrayValue(e->var_name, t, env);
            }
        }
        if (auto packed = buildPackedFlattenedPrefix(e->var_name, env)) {
            return packed;
        }
        if (!env.initialized.count(e->var_name) && env.symbols.count(e->var_name)) {
            env.error = "Read of uninitialized variable '" + e->var_name + "'";
            return nullptr;
        }
        return var;
    }
    case ExprKind::FieldAccess: {
        if (e->field_name.rfind("operator", 0) == 0) {
            return rewriteExpr(e->struct_base, env);
        }
        if (e->field_name == "setnext" || e->field_name == "call") {
            return rewriteExpr(e->struct_base, env);
        }
        std::string alias_object;
        std::vector<std::string> alias_fields;
        if (fieldAccessPath(e, alias_object, alias_fields)) {
            if (auto alias = env.alias_graph.resolvePath(alias_object, alias_fields)) {
                TypeInfo alias_type = e->type;
                if (env.symbols.count(alias->canonical_name)) alias_type = env.symbols[alias->canonical_name];
                return rewriteExpr(make_var(alias->canonical_name, alias_type), env);
            }
        }
        auto base = rewriteExpr(e->struct_base, env);
        if (!env.error.empty()) return nullptr;
        auto flat = baseName(base) + "_" + e->field_name;
        TypeInfo ty = e->type;
        if (env.symbols.count(flat)) ty = env.symbols[flat];
        env.symbols[flat] = ty;
        if (!env.initialized.count(flat) && env.initialized.count(baseName(base))) {
            env.initialized.insert(flat);
        }
        if (!ty.is_array && !env.initialized.count(flat)) {
            env.error = "Read of uninitialized field '" + flat + "'";
            return nullptr;
        }
        if (env.ssa_seed_symbols.count(baseName(base))) {
            env.ssa_seed_symbols[flat] = ty;
        }
        return make_var(flat, ty);
    }
    case ExprKind::ArrayAccess: {
        return rewriteArrayAccess(e, env);
    }
    case ExprKind::BinaryOp: {
        if (e->op == "/" || e->op == "%" || e->op == "/=" || e->op == "%=") {
            env.error = IntSemantics::binaryResultType(e->op.substr(0, 1), TypeInfo{}, TypeInfo{}).error;
            return nullptr;
        }
        auto l = rewriteExpr(e->left, env);
        auto r = rewriteExpr(e->right, env);
        if (!env.error.empty()) return nullptr;
        normalizeConstantOperandsForBinary(e->op, l, r);
        if ((e->op == "&" || e->op == "|" || e->op == "^") &&
            l && r && l->type.width > 0 && r->type.width > 0 &&
            l->type.width != r->type.width) {
            if (isWidthCastableConstantExpr(l)) {
                l = castIfWidthChanges(l, r->type);
            } else if (isWidthCastableConstantExpr(r)) {
                r = castIfWidthChanges(r, l->type);
            } else {
                env.error = IntSemantics::binaryResultType(e->op, l->type, r->type).error;
                return nullptr;
            }
        }
        if (auto folded = foldConstantOvershift(e->op, l, r)) return folded;
        auto out = make_binary(e->op, l, r, resultTypeForBinary(e->op, l ? l->type : TypeInfo{}, r ? r->type : TypeInfo{}));
        return out;
    }
    case ExprKind::UnaryOp: {
        if (e->op == "*" || e->op == "&") {
            env.error = "Unsupported pointer/reference operation '" + e->op + "'";
            return nullptr;
        }
        auto op = rewriteExpr(e->operand, env);
        if (!env.error.empty()) return nullptr;
        TypeInfo ty = e->type;
        if (e->op == "!" || e->op == "&&" || e->op == "||") ty = make_hw_type("bool", 1, false);
        else if ((e->op == "~" || e->op == "-") && op) ty = op->type;
        return make_unary(e->op, op, ty);
    }
    case ExprKind::Ternary: {
        auto c = rewriteExpr(e->cond, env);
        auto t = rewriteExpr(e->then_expr, env);
        auto f = rewriteExpr(e->else_expr, env);
        if (!env.error.empty()) return nullptr;
        normalizeConstantBranchesForTernary(t, f);
        TypeInfo out_type = e->type;
        if (t && f && t->type.width > 0 && t->type.width == f->type.width) {
            if (!t->type.hw_kind.empty()) {
                out_type = t->type;
            } else if (!f->type.hw_kind.empty()) {
                out_type = f->type;
            } else if (out_type.width != t->type.width) {
                out_type = t->type;
            }
        }
        return make_ite(c, t, f, out_type);
    }
    case ExprKind::Cast: {
        auto c = rewriteExpr(e->cast_expr, env);
        if (!env.error.empty()) return nullptr;
        return castIfWidthChanges(c, e->cast_type);
    }
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc: {
        auto c = rewriteExpr(e->cast_expr, env);
        if (!env.error.empty()) return nullptr;
        if (e->kind == ExprKind::SExt) return make_sext(c, e->to_width > 0 ? e->to_width : e->type.width);
        if (e->kind == ExprKind::Trunc) return make_trunc(c, e->to_width > 0 ? e->to_width : e->type.width, e->type.is_signed);
        return make_zext(c, e->to_width > 0 ? e->to_width : e->type.width);
    }
    case ExprKind::Slice:
    case ExprKind::BitSelect: {
        auto b = rewriteExpr(e->base, env);
        if (!env.error.empty()) return nullptr;
        int width = b ? b->type.width : 0;
        if (e->kind == ExprKind::BitSelect) {
            if (e->bit < 0 || (width > 0 && e->bit >= width)) {
                env.error = "Bit select out of bounds";
                return nullptr;
            }
            return make_bit_select(b, e->bit);
        }
        if (e->hi < 0 || e->lo < 0 || e->hi < e->lo || (width > 0 && e->hi >= width)) {
            env.error = "Slice out of bounds";
            return nullptr;
        }
        return make_slice(b, e->hi, e->lo, e->type);
    }
    case ExprKind::WriteSlice:
    case ExprKind::WriteBit: {
        ExprPtr b;
        if (e->base && e->base->kind == ExprKind::VarRef &&
            env.symbols.count(e->base->var_name) &&
            !env.initialized.count(e->base->var_name) &&
            !hasAnyBitInitialized(env, e->base->var_name)) {
            if (std::find(env.output_params.begin(), env.output_params.end(),
                          e->base->var_name) != env.output_params.end()) {
                b = zeroValueForType(env.symbols[e->base->var_name]);
            } else {
                b = cloneExpr(e->base);
                b->type = env.symbols[e->base->var_name];
            }
        } else {
            b = rewriteExpr(e->base, env);
        }
        auto v = rewriteExpr(e->value, env);
        if (!env.error.empty()) return nullptr;
        int base_width = b ? b->type.width : 0;
        if (e->kind == ExprKind::WriteBit) {
            if (e->bit < 0 || (base_width > 0 && e->bit >= base_width)) {
                env.error = "Bit assignment out of bounds";
                return nullptr;
            }
            return make_write_bit(b, e->bit, castIfWidthChanges(v, make_hw_type("bool", 1, false)), e->type);
        }
        if (e->hi < e->lo || e->hi < 0 || e->lo < 0 || (base_width > 0 && e->hi >= base_width)) {
            env.error = "Slice assignment out of bounds";
            return nullptr;
        }
        TypeInfo slice_ty = make_hw_type(e->type.is_signed ? "Int" : "UInt", e->hi - e->lo + 1, e->type.is_signed);
        return make_write_slice(b, e->hi, e->lo, castIfWidthChanges(v, slice_ty), e->type);
    }
    case ExprKind::Concat: {
        std::vector<ExprPtr> parts;
        for (auto& p : e->parts) {
            parts.push_back(rewriteExpr(p, env));
            if (!env.error.empty()) return nullptr;
        }
        return make_concat(std::move(parts));
    }
    case ExprKind::Repeat: {
        auto op = rewriteExpr(e->operand, env);
        if (!env.error.empty()) return nullptr;
        if (e->times <= 0) {
            env.error = "repeat count must be a positive compile-time constant";
            return nullptr;
        }
        return make_repeat(op, e->times);
    }
    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor: {
        auto op = rewriteExpr(e->operand, env);
        if (!env.error.empty()) return nullptr;
        return make_reduce(e->kind, op);
    }
    case ExprKind::Call:
        return inlineHelperCall(e, env);
    }
    return cloneExpr(e);
}

ExprPtr indexMatchExpr(const std::vector<ExprPtr>& indices,
                       const std::vector<int>& prefix) {
    ExprPtr cond;
    for (size_t i = 0; i < indices.size() && i < prefix.size(); ++i) {
        auto eq = make_binary("==",
            cloneExpr(indices[i]),
            make_literal(std::to_string(prefix[i]), indices[i] ? indices[i]->type : TypeInfo{"int", 32, true}),
            TypeInfo{"bool", 1, false});
        cond = cond ? make_binary("&&", cond, eq, TypeInfo{"bool", 1, false}) : eq;
    }
    return cond ? cond : make_literal("true", TypeInfo{"bool", 1, false});
}

void expandArrayWriteRec(const std::string& name,
                         const TypeInfo& arr_type,
                         const std::vector<ExprPtr>& indices,
                         ExprPtr value,
                         Env& env,
                         std::vector<StmtPtr>& out,
                         std::vector<int>& prefix,
                         size_t depth) {
    if (depth >= arr_type.array_dims.size()) {
        auto flat = joinIndexName(name, prefix);
        TypeInfo scalar = scalarTypeFromArray(arr_type);
        ExprPtr rhs = cloneExpr(value);
        bool all_literal = true;
        bool matches = true;
        for (size_t i = 0; i < indices.size() && i < prefix.size(); ++i) {
            auto lit = literalIndex(indices[i]);
            if (!lit.has_value()) {
                all_literal = false;
                break;
            }
            if (*lit != prefix[i]) matches = false;
        }
        if (all_literal && !matches) return;
        if (!all_literal) {
            auto old_value = make_var(flat, scalar);
            rhs = make_ite(indexMatchExpr(indices, prefix), rhs, old_value, scalar);
        }

        auto s = std::make_shared<Stmt>();
        s->kind = StmtKind::Assign;
        s->assign_target = make_var(flat, scalar);
        if (isRegProxyWdataAliasTarget(flat, env) && scalar.width > 0) {
            s->assign_value = make_write_slice(make_var(flat, scalar),
                                               scalar.width - 1,
                                               0,
                                               castIfWidthChanges(rhs, scalar),
                                               scalar);
        } else {
            s->assign_value = rhs;
        }
        out.push_back(s);
        env.symbols[flat] = scalar;
        env.initialized.insert(flat);
        return;
    }

    for (int i = 0; i < arr_type.array_dims[depth]; ++i) {
        prefix.push_back(i);
        expandArrayWriteRec(name, arr_type, indices, value, env, out, prefix, depth + 1);
        prefix.pop_back();
    }
}

std::vector<StmtPtr> rewriteArrayAssign(const StmtPtr& s, Env& env) {
    std::vector<StmtPtr> out;
    ExprPtr base;
    std::vector<ExprPtr> raw_indices;
    if (!collectArrayAccess(s->assign_target, base, raw_indices) || !base) {
        return out;
    }
    std::string name = baseName(base);
    if (name.empty()) {
        env.error = "Unsupported array assignment base expression";
        return out;
    }

    TypeInfo arr_type = base->type;
    if (env.symbols.count(name)) arr_type = env.symbols[name];
    if (arr_type.is_array && arr_type.array_dims.empty() && arr_type.array_size > 0) {
        arr_type.array_dims = {arr_type.array_size};
    }
    if (!arr_type.is_array || arr_type.array_dims.empty()) {
        env.error = "Assignment target '" + name + "' is not a static array";
        return out;
    }

    std::vector<ExprPtr> indices;
    bool all_literal = true;
    for (auto& idx : raw_indices) {
        indices.push_back(rewriteExpr(idx, env));
        if (!env.error.empty()) return out;
        if (!literalIndex(indices.back()).has_value()) all_literal = false;
    }
    if (!validateLiteralBounds(name, indices, arr_type.array_dims, env)) {
        return out;
    }
    auto value = rewriteExpr(s->assign_value, env);
    if (!env.error.empty()) return out;

    env.symbols[name] = arr_type;
    if (!all_literal) {
        std::vector<int> prefix;
        if (!allFlattenedElementsInitialized(name, arr_type, env, prefix, 0)) {
            env.error = "Dynamic array assignment to '" + name +
                "' requires all elements to be initialized";
            return out;
        }
    }
    std::vector<int> prefix;
    expandArrayWriteRec(name, arr_type, indices, value, env, out, prefix, 0);
    return out;
}

bool bodyEndsWithSwitchTerminator(const std::vector<StmtPtr>& body) {
    if (body.empty()) return false;
    auto last = body.back();
    if (!last) return false;
    if (last->kind == StmtKind::Break || last->kind == StmtKind::Return) return true;
    if (last->kind == StmtKind::Block) return bodyEndsWithSwitchTerminator(last->block_stmts);
    return false;
}

std::vector<StmtPtr> rewriteWholeArrayAssign(const StmtPtr& s, Env& env) {
    std::vector<StmtPtr> out;
    if (!s || !s->assign_target || s->assign_target->kind != ExprKind::VarRef ||
        !s->assign_value || s->assign_value->kind != ExprKind::VarRef) {
        return out;
    }
    std::string dst = s->assign_target->var_name;
    std::string src = s->assign_value->var_name;
    TypeInfo dst_type = s->assign_target->type;
    if (env.symbols.count(dst)) dst_type = env.symbols[dst];
    TypeInfo src_type = s->assign_value->type;
    if (env.symbols.count(src)) src_type = env.symbols[src];
    if (dst_type.is_array && dst_type.array_dims.empty() && dst_type.array_size > 0) dst_type.array_dims = {dst_type.array_size};
    if (src_type.is_array && src_type.array_dims.empty() && src_type.array_size > 0) src_type.array_dims = {src_type.array_size};
    if (!dst_type.is_array || !src_type.is_array || dst_type.array_dims != src_type.array_dims) return out;

    std::vector<int> prefix;
    std::string missing = firstUninitializedFlattenedElement(src, src_type, env, prefix, 0);
    if (!missing.empty()) {
        env.error = "Whole-array assignment from '" + src +
            "' reads uninitialized element '" + missing + "'";
        return out;
    }
    prefix.clear();
    std::function<void(size_t)> rec = [&](size_t depth) {
        if (depth >= dst_type.array_dims.size()) {
            auto stmt = std::make_shared<Stmt>();
            stmt->kind = StmtKind::Assign;
            auto scalar = scalarTypeFromArray(dst_type);
            auto d = joinIndexName(dst, prefix);
            auto v = joinIndexName(src, prefix);
            stmt->assign_target = make_var(d, scalar);
            stmt->assign_value = make_var(v, scalar);
            out.push_back(stmt);
            env.symbols[d] = scalar;
            env.initialized.insert(d);
            return;
        }
        for (int i = 0; i < dst_type.array_dims[depth]; ++i) {
            prefix.push_back(i);
            rec(depth + 1);
            prefix.pop_back();
        }
    };
    rec(0);
    return out;
}

StmtPtr rewriteBitSliceAssign(const StmtPtr& s, Env& env) {
    if (!s || !s->assign_target) return nullptr;
    auto target_call = s->assign_target;
    if (target_call->kind == ExprKind::VarRef) {
        std::string text = target_call->var_name;
        text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
            return std::isspace(c);
        }), text.end());
        if (!text.empty() && text.front() == '(') {
            auto close_base = text.find(")(");
            if (close_base != std::string::npos) {
                text = text.substr(1, close_base - 1) + text.substr(close_base + 1);
            }
        }
        auto lp = text.find('(');
        auto rp = text.rfind(')');
        if (lp != std::string::npos && rp == text.size() - 1 && lp > 0) {
            std::string base_name = text.substr(0, lp);
            while (base_name.size() >= 2 && base_name.front() == '(' && base_name.back() == ')') {
                base_name = base_name.substr(1, base_name.size() - 2);
            }
            auto sym = env.symbols.find(base_name);
            if (sym != env.symbols.end() && sym->second.is_hw_int) {
                std::string inside = text.substr(lp + 1, rp - lp - 1);
                auto comma = inside.find(',');
                auto parse_int = [](const std::string& s, int& out) {
                    if (s.empty() || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); })) {
                        return false;
                    }
                    out = std::stoi(s);
                    return true;
                };
                if (comma == std::string::npos) {
                    int bit = -1;
                    if (parse_int(inside, bit)) target_call = make_bit_select(make_var(base_name, sym->second), bit);
                } else {
                    int hi = -1;
                    int lo = -1;
                    if (parse_int(inside.substr(0, comma), hi) && parse_int(inside.substr(comma + 1), lo)) {
                        target_call = make_slice(make_var(base_name, sym->second),
                                                 hi,
                                                 lo,
                                                 make_hw_type(sym->second.is_signed ? "Int" : "UInt",
                                                              hi - lo + 1,
                                                              sym->second.is_signed));
                    }
                }
            }
        }
        if (allowUnsafeTextFallback && target_call->kind == ExprKind::VarRef) {
            if (auto parsed = parseOpaqueHwMethodVar(target_call, env)) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
                if (parsed->kind == ExprKind::BitSelect || parsed->kind == ExprKind::Slice) {
                    target_call = parsed;
                }
            }
            if (!env.error.empty()) return nullptr;
        }
    }
    bool is_bit = false;
    bool is_slice = false;
    ExprPtr base_expr;
    int hi = -1;
    int lo = -1;
    if (target_call->kind == ExprKind::Call &&
        (target_call->callee == "__bit" || target_call->callee == "__slice")) {
        is_bit = target_call->callee == "__bit";
        is_slice = target_call->callee == "__slice";
        if (target_call->args.empty()) return nullptr;
        base_expr = target_call->args.front();
        if (is_bit && target_call->args.size() >= 2) hi = lo = literalIntValue(target_call->args[1], -1);
        if (is_slice && target_call->args.size() >= 3) {
            hi = literalIntValue(target_call->args[1], -1);
            lo = literalIntValue(target_call->args[2], -1);
        }
    } else if (target_call->kind == ExprKind::BitSelect || target_call->kind == ExprKind::Slice) {
        is_bit = target_call->kind == ExprKind::BitSelect;
        is_slice = target_call->kind == ExprKind::Slice;
        base_expr = target_call->base;
        hi = is_bit ? target_call->bit : target_call->hi;
        lo = is_bit ? target_call->bit : target_call->lo;
    } else {
        return nullptr;
    }
    if (!base_expr || base_expr->kind != ExprKind::VarRef) {
        env.error = "Unsupported bit/slice assignment base expression";
        return nullptr;
    }
    std::string base = base_expr->var_name;
    TypeInfo base_type = env.symbols.count(base) ? env.symbols[base] : base_expr->type;
    bool base_has_value = env.initialized.count(base) || hasAnyBitInitialized(env, base);
    ExprPtr base_value_override;
    if (!base_has_value &&
        std::find(env.output_params.begin(), env.output_params.end(), base) != env.output_params.end()) {
        // Do not use the incoming version of a pure output as the base for
        // first write_slice/write_bit. The hardware meaning is an update from
        // an explicit zero/false value unless a previous assignment in this
        // function has already produced a value.
        base_value_override = zeroValueForType(base_type);
        base_has_value = true;
    }
    auto value = rewriteExpr(s->assign_value, env);
    if (!env.error.empty()) return nullptr;
    if (!base_has_value && (hi < 0 || lo < 0)) {
        env.error = "Dynamic bit/slice assignment to uninitialized '" + base +
            "' requires a previous value";
        return nullptr;
    }
    if (hi < lo || hi < 0 || lo < 0 || (base_type.width > 0 && hi >= base_type.width)) {
        env.error = is_bit ? "Bit assignment out of bounds" : "Slice assignment out of bounds";
        return nullptr;
    }
    ExprPtr write_expr;
    auto base_var = base_value_override ? base_value_override : make_var(base, base_type);
    if (is_bit) {
        write_expr = make_write_bit(base_var, hi, castIfWidthChanges(value, make_hw_type("bool", 1, false)), base_type);
    } else {
        TypeInfo slice_ty = make_hw_type(value && value->type.is_signed ? "Int" : "UInt", hi - lo + 1, value ? value->type.is_signed : false);
        write_expr = make_write_slice(base_var, hi, lo, castIfWidthChanges(value, slice_ty), base_type);
    }

    auto out = std::make_shared<Stmt>();
    out->kind = StmtKind::Assign;
    out->assign_target = make_var(base, base_type);
    out->assign_value = write_expr;
    env.symbols[base] = base_type;
    if (hi >= 0 && lo >= 0) {
        markBitRangeInitialized(env, base, base_type, hi, lo);
        if (!env.error.empty()) return nullptr;
    } else {
        markScalarFullyInitialized(env, base, base_type);
    }
    return out;
}

ExprPtr rewriteTarget(const ExprPtr& e, Env& env) {
    if (!e) return nullptr;
    if (e->kind == ExprKind::VarRef) {
        auto out = cloneExpr(e);
        auto it = env.symbols.find(out->var_name);
        if (it != env.symbols.end()) out->type = it->second;
        return out;
    }
    if (e->kind == ExprKind::FieldAccess) {
        std::string alias_object;
        std::vector<std::string> alias_fields;
        if (fieldAccessPath(e, alias_object, alias_fields)) {
            if (auto alias = env.alias_graph.resolvePath(alias_object, alias_fields)) {
                if (!alias->writable) {
                    env.error = "Unsupported write through const alias '" + alias_object + "." + e->field_name + "'";
                    return nullptr;
                }
                TypeInfo alias_type = e->type;
                if (env.symbols.count(alias->canonical_name)) alias_type = env.symbols[alias->canonical_name];
                markFormalWrite(env, alias->canonical_name);
                return make_var(alias->canonical_name, alias_type);
            }
        }
        auto b = rewriteTarget(e->struct_base, env);
        return make_var(baseName(b) + "_" + e->field_name, e->type);
    }
    if (e->kind == ExprKind::UnaryOp && e->op == "*" && e->operand &&
        e->operand->kind == ExprKind::VarRef) {
        return make_var(e->operand->var_name, e->type);
    }
    if (e->kind == ExprKind::ArrayAccess) {
        env.error = "Array element assignment is not supported after flattening";
        return nullptr;
    }
    env.error = "Unsupported assignment target";
    return nullptr;
}

StmtPtr rewriteStmt(const StmtPtr& s, Env& env) {
    if (!s || !env.error.empty()) return nullptr;
    auto r = std::make_shared<Stmt>(*s);
    switch (s->kind) {
    case StmtKind::Decl:
        env.symbols[s->decl_name] = s->decl_type;
        if (isRegProxyType(s->decl_type) || isReqHelperType(s->decl_type)) {
            if (isRegProxyType(s->decl_type)) {
                if (s->decl_init_args.size() < 3) {
                    env.error = "Unsupported RegProxy declaration without constructor aliases for '" + s->decl_name + "'";
                    return nullptr;
                }
                Env::RegProxyAlias alias{
                    directVarName(s->decl_init_args[0]),
                    directVarName(s->decl_init_args[1]),
                    directVarName(s->decl_init_args[2])
                };
                if (alias.rdata.empty() || alias.wen.empty() || alias.wdata.empty()) {
                    env.error = "Unsupported RegProxy declaration with non-variable constructor alias for '" + s->decl_name + "'";
                    return nullptr;
                }
                env.regproxy_aliases[s->decl_name] = alias;
                env.alias_graph.bindField(s->decl_name, "rdata", AliasTarget{alias.rdata, AliasKind::RegProxyPort, false});
                env.alias_graph.bindField(s->decl_name, "wen", AliasTarget{alias.wen, AliasKind::RegProxyPort, true});
                env.alias_graph.bindField(s->decl_name, "wdata", AliasTarget{alias.wdata, AliasKind::RegProxyPort, true});
                markFormalWrite(env, alias.wen);
                markFormalWrite(env, alias.wdata);
                env.output_default_reasons[alias.wen] = "write_enable_default_false";
                env.output_default_reasons[alias.wdata] = "wdata_default_zero_when_wen_false";
                env.output_paired_controls[alias.wdata] = alias.wen;
            } else {
                if (s->decl_init_args.size() < 2) {
                    env.error = "Unsupported ReqHelper declaration without output aliases for '" + s->decl_name + "'";
                    return nullptr;
                }
                std::string arg_data = directVarName(s->decl_init_args[1]);
                std::string vld = directVarName(s->decl_init_args[0]);
                Env::ReqHelperAlias alias{
                    vld,
                    arg_data
                };
                if (alias.vld.empty() || alias.arg_data.empty()) {
                    env.error = "Unsupported ReqHelper declaration with non-variable output alias for '" + s->decl_name + "'";
                    return nullptr;
                }
                markFormalWrite(env, alias.vld);
                markFormalWrite(env, alias.arg_data);
                env.output_default_reasons[alias.vld] = "valid_default_false";
                env.output_default_reasons[alias.arg_data] = "payload_default_zero_when_valid_false";
                env.output_paired_controls[alias.arg_data] = alias.vld;
                env.reqhelper_aliases[s->decl_name] = alias;
                env.alias_graph.bindField(s->decl_name, "vld_ports", AliasTarget{alias.vld, AliasKind::ReqHelperPort, true});
                env.alias_graph.bindField(s->decl_name, "arg_data", AliasTarget{alias.arg_data, AliasKind::ReqHelperPort, true});
                env.alias_graph.bindField(s->decl_name, "arg_s", AliasTarget{alias.arg_data, AliasKind::ReqHelperPort, true});
                TypeInfo valid_type = symbolType(env, alias.vld, TypeInfo{"bool", 1, false});
                env.initialized.insert(alias.vld);
                markScalarFullyInitialized(env, s->decl_name, s->decl_type);
                addSeedSymbol(env, alias.vld, valid_type);
                auto init_valid = std::make_shared<Stmt>();
                init_valid->kind = StmtKind::Assign;
                init_valid->assign_target = make_var(alias.vld, valid_type);
                init_valid->assign_value = make_literal("false", valid_type);
                return init_valid;
            }
            markScalarFullyInitialized(env, s->decl_name, s->decl_type);
            r->decl_init = std::nullopt;
            return r;
        }
        if (s->decl_type.is_array) {
            TypeInfo arr_type = s->decl_type;
            if (arr_type.array_dims.empty() && arr_type.array_size > 0) arr_type.array_dims = {arr_type.array_size};
            env.symbols[s->decl_name] = arr_type;
            if (!arr_type.array_dims.empty()) {
                std::vector<int> prefix;
                addFlattenedArraySymbols(env, s->decl_name, arr_type,
                    !arr_type.init_values.empty(), prefix, 0);
            }
            if (arr_type.is_static && !arr_type.init_values.empty()) {
                env.lookup_tables[s->decl_name] = arr_type.init_values;
                env.initialized.insert(s->decl_name);
                return nullptr;
            }
        }
        if (!s->decl_type.struct_name.empty() && !s->decl_init_args.empty()) {
            auto fields = findStructFields(s->decl_type, env);
            if (fields) {
                auto bind_field_alias = [&](const StructFieldInfo& field, size_t arg_index) -> bool {
                    if (!field.type.is_reference && !field.type.is_pointer) return true;
                    if (arg_index >= s->decl_init_args.size()) {
                        env.error = "Unsupported constructor reference alias for '" +
                                    s->decl_name + "." + field.name + "'";
                        return false;
                    }
                    std::string actual = directVarName(s->decl_init_args[arg_index]);
                    if (actual.empty()) {
                        env.error = "Unsupported constructor reference alias for '" +
                                    s->decl_name + "." + field.name + "'";
                        return false;
                    }
                    if (!env.symbols.count(actual)) {
                        env.error = "Unknown constructor alias target '" + actual + "'";
                        return false;
                    }
                    bool writable = field.type.is_pointer || !field.type.is_const;
                    AliasTarget target{actual,
                                       field.type.is_pointer ? AliasKind::Pointer :
                                           (writable ? AliasKind::MutableRef : AliasKind::ConstRef),
                                       writable};
                    target.reason = "constructor_member_initializer";
                    env.alias_graph.bindField(s->decl_name, field.name, std::move(target));
                    return true;
                };

                auto ctors = findStructConstructors(s->decl_type, env);
                const StructConstructorInfo* selected_ctor = nullptr;
                if (ctors) {
                    for (const auto& ctor : *ctors) {
                        if (ctor.param_names.size() == s->decl_init_args.size()) {
                            selected_ctor = &ctor;
                            break;
                        }
                    }
                    if (!selected_ctor) {
                        env.error = "Unsupported constructor alias mapping for '" + s->decl_name +
                                    "': no constructor initializer matches argument count";
                        return nullptr;
                    }
                }

                if (selected_ctor) {
                    for (const auto& field : *fields) {
                        if (!field.type.is_reference && !field.type.is_pointer) continue;
                        auto mapped = selected_ctor->field_to_param.find(field.name);
                        if (mapped == selected_ctor->field_to_param.end()) {
                            env.error = "Unsupported constructor alias mapping for '" +
                                        s->decl_name + "." + field.name + "'";
                            return nullptr;
                        }
                        auto param_it = std::find(selected_ctor->param_names.begin(),
                                                  selected_ctor->param_names.end(),
                                                  mapped->second);
                        if (param_it == selected_ctor->param_names.end()) {
                            env.error = "Unsupported constructor alias parameter '" + mapped->second + "'";
                            return nullptr;
                        }
                        size_t arg_index = static_cast<size_t>(std::distance(selected_ctor->param_names.begin(), param_it));
                        if (!bind_field_alias(field, arg_index)) return nullptr;
                    }
                } else {
                    size_t count = std::min(fields->size(), s->decl_init_args.size());
                    for (size_t i = 0; i < count; ++i) {
                        const auto& field = (*fields)[i];
                        if (!bind_field_alias(field, i)) return nullptr;
                    }
                }
            }
        }
        if (s->decl_init.has_value() && !s->decl_type.is_array) {
            r->decl_init = rewriteExpr(s->decl_init.value(), env);
            r->decl_init = castIfWidthChanges(r->decl_init.value(), s->decl_type);
            markScalarFullyInitialized(env, s->decl_name, s->decl_type);
        }
        return r;
    case StmtKind::Assign: {
        if (auto bit_assign = rewriteBitSliceAssign(s, env)) {
            return bit_assign;
        }
        if (!env.error.empty()) return nullptr;
        auto value = rewriteExpr(s->assign_value, env);
        auto target = rewriteTarget(s->assign_target, env);
        if (!env.error.empty()) return nullptr;
        value = castIfWidthChanges(value, target->type);
        std::string name = targetName(target);
        if (!name.empty()) {
            env.symbols[name] = target->type;
            if (env.param_directions.count(name)) {
                markFormalWrite(env, name);
            }
            markScalarFullyInitialized(env, name, target->type);
        }
        r->assign_target = target;
        if (!name.empty() && isRegProxyWdataAliasTarget(name, env) && target->type.width > 0) {
            value = make_write_slice(make_var(name, target->type),
                                     target->type.width - 1,
                                     0,
                                     castIfWidthChanges(value, target->type),
                                     target->type);
        }
        r->assign_value = value;
        return r;
    }
    case StmtKind::If: {
        r->if_cond = rewriteExpr(s->if_cond, env);
        if (!env.error.empty()) return nullptr;
        Env then_env = env;
        Env else_env = env;
        r->if_then = rewriteStmts(s->if_then, then_env);
        r->if_else = rewriteStmts(s->if_else, else_env);
        if (!then_env.error.empty()) env.error = then_env.error;
        if (!else_env.error.empty()) env.error = else_env.error;
        if (!env.error.empty()) return nullptr;
        std::unordered_set<std::string> merged = env.initialized;
        for (auto& v : then_env.initialized) {
            if (else_env.initialized.count(v)) merged.insert(v);
        }
        env.initialized = std::move(merged);
        env.symbols.insert(then_env.symbols.begin(), then_env.symbols.end());
        env.symbols.insert(else_env.symbols.begin(), else_env.symbols.end());
        env.lookup_tables.insert(then_env.lookup_tables.begin(), then_env.lookup_tables.end());
        env.lookup_tables.insert(else_env.lookup_tables.begin(), else_env.lookup_tables.end());
        return r;
    }
    case StmtKind::Switch:
        r->switch_expr = rewriteExpr(s->switch_expr, env);
        {

        bool has_default = false;
        bool first_case = true;
        std::unordered_set<std::string> merged;
        for (auto& c : r->switch_cases) {
            if (c.value.has_value()) c.value = rewriteExpr(c.value.value(), env);
            else has_default = true;
            Env case_env = env;
            c.body = rewriteStmts(c.body, case_env);
            if (!case_env.error.empty()) {
                env.error = case_env.error;
                return nullptr;
            }
            if (first_case) {
                merged = case_env.initialized;
                first_case = false;
            } else {
                for (auto it = merged.begin(); it != merged.end();) {
                    if (!case_env.initialized.count(*it)) it = merged.erase(it);
                    else ++it;
                }
            }
            env.symbols.insert(case_env.symbols.begin(), case_env.symbols.end());
            env.lookup_tables.insert(case_env.lookup_tables.begin(), case_env.lookup_tables.end());
        }
        if (has_default && !first_case) env.initialized = std::move(merged);
        }
        return r;
    case StmtKind::Block:
        r->block_stmts = rewriteStmts(s->block_stmts, env);
        return r;
    case StmtKind::For:
        r->for_body = rewriteStmts(s->for_body, env);
        if (s->for_init) r->for_init = rewriteStmt(s->for_init, env);
        r->for_cond = rewriteExpr(s->for_cond, env);
        r->for_step = rewriteExpr(s->for_step, env);
        return r;
    case StmtKind::While:
    case StmtKind::DoWhile:
        env.error = "Unsupported dynamic loop; use statically bounded for-loop";
        return nullptr;
    case StmtKind::Return:
        if (s->return_value.has_value()) r->return_value = rewriteExpr(s->return_value.value(), env);
        return r;
    case StmtKind::ExprStmt:
        r->expr_stmt = rewriteExpr(s->expr_stmt, env);
        return r;
    default:
        return r;
    }
}

std::vector<StmtPtr> rewriteStmts(const std::vector<StmtPtr>& stmts, Env& env) {
    std::vector<StmtPtr> out;
    for (auto& s : stmts) {
        if (s && s->kind == StmtKind::Decl && s->decl_type.is_array &&
            !s->decl_type.init_values.empty() && !s->decl_type.is_static) {
            auto expanded = rewriteArrayInitDecl(s, env);
            if (!env.error.empty()) break;
            if (!expanded.empty()) {
                out.insert(out.end(), expanded.begin(), expanded.end());
                continue;
            }
        }
        if (s && s->kind == StmtKind::Decl && s->decl_type.is_array &&
            s->decl_init.has_value()) {
            auto expanded = rewritePackedArrayInitDecl(s, env);
            if (!env.error.empty()) break;
            if (!expanded.empty()) {
                out.insert(out.end(), expanded.begin(), expanded.end());
                continue;
            }
        }
        if (s && s->kind == StmtKind::ExprStmt && s->expr_stmt &&
            s->expr_stmt->kind == ExprKind::Call) {
            auto inlined = inlineProcedureCall(s->expr_stmt, env);
            if (!env.error.empty()) break;
            auto rewritten = rewriteStmts(inlined, env);
            if (!env.error.empty()) break;
            out.insert(out.end(), rewritten.begin(), rewritten.end());
            continue;
        }
        if (s && s->kind == StmtKind::Assign && s->assign_target &&
            s->assign_target->kind == ExprKind::ArrayAccess) {
            auto expanded = rewriteArrayAssign(s, env);
            if (!env.error.empty()) break;
            out.insert(out.end(), expanded.begin(), expanded.end());
            continue;
        }
        if (s && s->kind == StmtKind::Assign) {
            auto expanded = rewriteWholeArrayAssign(s, env);
            if (!expanded.empty()) {
                out.insert(out.end(), expanded.begin(), expanded.end());
                continue;
            }
        }
        auto r = rewriteStmt(s, env);
        if (!env.error.empty()) break;
        if (r) out.push_back(r);
    }
    return out;
}

} // namespace

NormalizeResult normalizeFunction(const FunctionAST& func,
                                  const std::vector<StmtPtr>& body) {
    NormalizeResult result;
    Env env;
    env.struct_fields = func.struct_fields;
    env.struct_constructors = func.struct_constructors;
    for (auto& h : func.helpers) {
        if (h) env.helpers[h->name] = *h;
    }
    for (auto& [name, lambda] : func.lambdas) {
        if (lambda) env.lambdas[name] = *lambda;
    }

    for (auto& p : func.params) {
        std::string pname = p.name;
        env.symbols[pname] = p.type;
        env.param_directions[pname] = paramDirectionName(p.direction);
        bool is_output = isOutputParam(p);
        bool is_input = isInputParam(p);
        if (is_output) markFormalWrite(env, pname);
        if (is_input) {
            markScalarFullyInitialized(env, pname, p.type);
            env.input_arrays.insert(pname);
            addSeedSymbol(env, pname, p.type);
        }
        TypeInfo param_type = p.type;
        if (param_type.is_array && param_type.array_dims.empty() && param_type.array_size > 0) {
            param_type.array_dims = {param_type.array_size};
            env.symbols[pname] = param_type;
        }
        if (param_type.is_array && !param_type.array_dims.empty()) {
            std::vector<int> prefix;
            bool input_array_by_value = p.passing == ParamPassingKind::Value ||
                                        p.passing == ParamPassingKind::ConstRef;
            bool initialized_array = is_input || input_array_by_value;
            addFlattenedArraySymbols(env, pname, param_type, initialized_array, prefix, 0);
            if (initialized_array) {
                env.input_arrays.insert(pname);
                env.initialized.insert(pname);
                addSeedSymbolsWithPrefix(env, pname);
            }
        }
        if (!param_type.struct_name.empty()) {
            addStructFieldSymbols(pname, param_type, env, is_input);
            if (is_input) addSeedSymbolsWithPrefix(env, pname);
        }
    }

    result.body = rewriteStmts(body, env);
    result.error = env.error;
    result.symbols = env.symbols;
    result.ssa_seed_symbols = env.ssa_seed_symbols;
    result.lookup_tables = env.lookup_tables;
    result.output_params = env.output_params;
    result.param_directions = env.param_directions;
    result.output_default_reasons = env.output_default_reasons;
    result.output_paired_controls = env.output_paired_controls;
    return result;
}

} // namespace pred

