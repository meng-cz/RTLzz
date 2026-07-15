#include "s2validate/S2Validate.h"

#include <algorithm>
#include <functional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pred::s2validate {
namespace {

enum class FunctionKind {
    Top,
    Helper,
    Lambda,
};

struct FunctionView {
    const FunctionAST* fn = nullptr;
    FunctionKind kind = FunctionKind::Helper;
    std::string name;
    std::size_t id = 0;
};

struct Scope {
    std::unordered_map<std::string, TypeInfo> symbols;
};

struct Context {
    const FunctionAST& top;
    ValidateOptions options;
    std::vector<ValidateWarning> warnings;
    std::vector<FunctionView> functions;
    std::unordered_map<std::string, std::vector<std::size_t>> functions_by_name;
    std::unordered_map<std::string, std::vector<StructFieldInfo>> struct_fields;
    std::unordered_map<std::size_t, std::unordered_set<std::size_t>> call_graph;
};

std::string kindName(FunctionKind kind) {
    switch (kind) {
    case FunctionKind::Top: return "top";
    case FunctionKind::Helper: return "helper";
    case FunctionKind::Lambda: return "lambda";
    }
    return "function";
}

std::string canonicalName(std::string name) {
    if (name.rfind("struct ", 0) == 0) name = name.substr(7);
    if (name.rfind("class ", 0) == 0) name = name.substr(6);
    auto lt = name.find('<');
    if (lt != std::string::npos) name = name.substr(0, lt);
    return name;
}

std::string typeLabel(const TypeInfo& type) {
    std::string out = !type.struct_name.empty() ? type.struct_name :
        (!type.name.empty() ? type.name : type.hw_kind);
    if (out.empty()) out = "<anon>";
    if (type.width > 0 && out.find('<') == std::string::npos &&
        out != "bool" && out != "void") {
        out += "<" + std::to_string(type.width) + ">";
    }
    if (type.is_array) {
        if (!type.array_dims.empty()) {
            for (int dim : type.array_dims) out += "[" + std::to_string(dim) + "]";
        } else if (type.array_size > 0) {
            out += "[" + std::to_string(type.array_size) + "]";
        } else {
            out += "[]";
        }
    }
    if (type.is_const) out = "const " + out;
    if (type.is_reference) out += "&";
    if (type.is_pointer) out += "*";
    return out;
}

std::string signatureKey(const FunctionAST& fn) {
    std::string out = fn.name + "/" + std::to_string(fn.params.size());
    for (const auto& param : fn.params) out += "/" + typeLabel(param.type);
    return out;
}

bool isVoidType(const TypeInfo& type) {
    return type.name == "void" && !type.is_array && type.struct_name.empty();
}

bool isBoolType(const TypeInfo& type) {
    return (type.name == "bool" || type.hw_kind == "bool") &&
           !type.is_array && type.struct_name.empty();
}

bool isProxyCarrierType(const TypeInfo& type) {
    const std::string text = type.name + " " + type.struct_name;
    return text.find("__RegProxy") != std::string::npos ||
           text.find("__ReqHelper") != std::string::npos ||
           text.find("QueueProxy") != std::string::npos ||
           text.find("__BRAMProxy") != std::string::npos;
}

[[noreturn]] void fail(const std::string& message, DebugLoc loc = {}) {
    ErrorContext context;
    context.loc = std::move(loc);
    throwRTLZZ(std::move(context), message);
}

[[noreturn]] void failAt(const std::string& stage,
                         DebugLoc loc,
                         const std::string& message,
                         const std::string& note = {}) {
    ErrorContext context;
    context.stage = stage;
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    context.note = note;
    throwRTLZZ(std::move(context), message);
}

bool sameTypeShallow(const TypeInfo& lhs, const TypeInfo& rhs) {
    return lhs.width == rhs.width &&
           lhs.is_signed == rhs.is_signed &&
           lhs.hw_kind == rhs.hw_kind &&
           canonicalName(lhs.name) == canonicalName(rhs.name) &&
           canonicalName(lhs.struct_name) == canonicalName(rhs.struct_name) &&
           lhs.is_array == rhs.is_array &&
           lhs.array_size == rhs.array_size &&
           lhs.array_dims == rhs.array_dims &&
           lhs.is_pointer == rhs.is_pointer &&
           lhs.is_reference == rhs.is_reference;
}

bool isSupportedScalar(const TypeInfo& type) {
    if (type.is_array || type.is_pointer || type.is_reference ||
        !type.struct_name.empty()) {
        return false;
    }
    if (isBoolType(type)) return true;
    if (type.width <= 0) return false;
    if (type.hw_kind == "Int" || type.hw_kind == "UInt" ||
        type.hw_kind == "builtin") {
        return true;
    }
    if (type.name.rfind("Int<", 0) == 0 ||
        type.name.rfind("UInt<", 0) == 0 ||
        type.name == "int" || type.name == "unsigned int" ||
        type.name == "uint8_t" || type.name == "int8_t" ||
        type.name == "uint16_t" || type.name == "int16_t" ||
        type.name == "uint32_t" || type.name == "int32_t" ||
        type.name == "uint64_t" || type.name == "int64_t") {
        return true;
    }
    return false;
}

int arrayExtent(const TypeInfo& type) {
    if (!type.is_array) return 0;
    if (!type.array_dims.empty()) return type.array_dims.front();
    return type.array_size;
}

TypeInfo arrayElement(TypeInfo type) {
    if (!type.is_array) return type;
    if (!type.array_dims.empty()) {
        type.array_dims.erase(type.array_dims.begin());
        if (type.array_dims.empty()) {
            type.is_array = false;
            type.array_size = 0;
        } else {
            type.array_size = type.array_dims.front();
        }
        return type;
    }
    type.is_array = false;
    type.array_size = 0;
    return type;
}

const std::vector<StructFieldInfo>* findStructFields(const Context& ctx,
                                                     const TypeInfo& type) {
    std::vector<std::string> keys;
    if (!type.struct_name.empty()) keys.push_back(type.struct_name);
    if (!type.name.empty()) keys.push_back(type.name);
    if (!type.struct_name.empty()) keys.push_back(canonicalName(type.struct_name));
    if (!type.name.empty()) keys.push_back(canonicalName(type.name));
    for (const auto& key : keys) {
        if (key.empty()) continue;
        auto it = ctx.struct_fields.find(key);
        if (it != ctx.struct_fields.end()) return &it->second;
        auto prefixed = "struct " + key;
        it = ctx.struct_fields.find(prefixed);
        if (it != ctx.struct_fields.end()) return &it->second;
    }
    return nullptr;
}

void validateType(const Context& ctx,
                  const TypeInfo& type,
                  bool allow_reference,
                  bool allow_aggregate,
                  DebugLoc loc,
                  std::unordered_set<std::string>& visiting);

void validateStructType(const Context& ctx,
                        const TypeInfo& type,
                        DebugLoc loc,
                        std::unordered_set<std::string>& visiting) {
    if (isProxyCarrierType(type)) {
        failAt("s2validate", std::move(loc),
               "Unsupported legacy proxy carrier type '" + typeLabel(type) + "'");
    }
    const auto* fields = findStructFields(ctx, type);
    if (!fields) {
        failAt("s2validate", std::move(loc),
               "Missing struct metadata for type '" + typeLabel(type) + "'");
    }
    if (fields->empty()) {
        failAt("s2validate", std::move(loc),
               "Empty struct type is not supported: '" + typeLabel(type) + "'");
    }
    std::string key = canonicalName(!type.struct_name.empty() ? type.struct_name : type.name);
    if (key.empty()) key = typeLabel(type);
    if (visiting.count(key)) return;
    visiting.insert(key);
    for (const auto& field : *fields) {
        if (field.type.is_reference || field.type.is_pointer) {
            failAt("s2validate", loc,
                   "Struct field '" + key + "." + field.name +
                       "' may not be reference or pointer type");
        }
        validateType(ctx, field.type, false, true, loc, visiting);
    }
    visiting.erase(key);
}

void validateType(const Context& ctx,
                  const TypeInfo& type,
                  bool allow_reference,
                  bool allow_aggregate,
                  DebugLoc loc,
                  std::unordered_set<std::string>& visiting) {
    if (isProxyCarrierType(type)) {
        failAt("s2validate", std::move(loc),
               "Unsupported legacy proxy carrier type '" + typeLabel(type) + "'");
    }
    if (type.is_pointer) {
        failAt("s2validate", std::move(loc),
               "Pointer type is not supported: '" + typeLabel(type) + "'");
    }
    if (type.is_reference && !allow_reference) {
        failAt("s2validate", std::move(loc),
               "Reference type is not supported here: '" + typeLabel(type) + "'");
    }
    TypeInfo value_type = type;
    value_type.is_reference = false;
    if (value_type.is_array) {
        if (arrayExtent(value_type) <= 0) {
            failAt("s2validate", std::move(loc),
                   "Dynamic or unknown-size array is not supported: '" +
                       typeLabel(type) + "'");
        }
        auto elem = arrayElement(value_type);
        if (elem.is_pointer || elem.is_reference) {
            failAt("s2validate", std::move(loc),
                   "Array element may not be reference or pointer type: '" +
                       typeLabel(type) + "'");
        }
        validateType(ctx, elem, false, allow_aggregate, loc, visiting);
        return;
    }
    if (!value_type.struct_name.empty()) {
        if (!allow_aggregate) {
            failAt("s2validate", std::move(loc),
                   "Aggregate type is not supported here: '" + typeLabel(type) + "'");
        }
        validateStructType(ctx, value_type, loc, visiting);
        return;
    }
    if (!isVoidType(value_type) && !isSupportedScalar(value_type)) {
        failAt("s2validate", std::move(loc),
               "Unsupported or unknown-width scalar type: '" + typeLabel(type) + "'");
    }
}

void validateTopPortType(const Context& ctx, const ParamDecl& param) {
    TypeInfo type = param.type;
    type.is_reference = false;
    if (type.is_pointer) {
        failAt("s2validate", param.debug_loc,
               "Top-level pointer parameter is not supported: '" + param.name + "'");
    }
    if (!type.struct_name.empty()) {
        failAt("s2validate", param.debug_loc,
               "Top-level struct parameter is not supported: '" + param.name + "'");
    }
    if (type.is_array) {
        if (arrayExtent(type) <= 0) {
            failAt("s2validate", param.debug_loc,
                   "Top-level array parameter must have static dimensions: '" +
                       param.name + "'");
        }
        TypeInfo elem = arrayElement(type);
        while (elem.is_array) {
            if (arrayExtent(elem) <= 0) {
                failAt("s2validate", param.debug_loc,
                       "Top-level array parameter must have static dimensions: '" +
                           param.name + "'");
            }
            elem = arrayElement(elem);
        }
        if (!isSupportedScalar(elem)) {
            failAt("s2validate", param.debug_loc,
                   "Top-level array element type must be bool or fixed-width integer: '" +
                       param.name + "'");
        }
        return;
    }
    if (!isSupportedScalar(type)) {
        failAt("s2validate", param.debug_loc,
               "Top-level parameter type must be bool or fixed-width integer: '" +
                   param.name + "'");
    }
}

void declareSymbol(std::vector<Scope>& scopes,
                   const std::string& name,
                   const TypeInfo& type,
                   DebugLoc loc) {
    if (name.empty()) {
        failAt("s2validate", std::move(loc), "Encountered unnamed declaration");
    }
    auto& current = scopes.back().symbols;
    if (current.count(name)) {
        failAt("s2validate", std::move(loc),
               "Redefinition in the same lexical scope: '" + name + "'");
    }
    current[name] = type;
}

std::optional<TypeInfo> lookupSymbol(const std::vector<Scope>& scopes,
                                     const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto sym = it->symbols.find(name);
        if (sym != it->symbols.end()) return sym->second;
    }
    return std::nullopt;
}

bool isAllowedConstructorCall(const Context& ctx, const ExprPtr& expr) {
    if (!expr || expr->kind != ExprKind::Call) return false;
    if (expr->callee == "Int" || expr->callee == "UInt" ||
        expr->callee.rfind("Int<", 0) == 0 ||
        expr->callee.rfind("UInt<", 0) == 0 ||
        expr->callee == "bool") {
        return true;
    }
    if (!expr->type.struct_name.empty()) {
        std::string callee = canonicalName(expr->callee);
        std::string type_name = canonicalName(expr->type.struct_name);
        if (callee == type_name) return true;
        return findStructFields(ctx, expr->type) != nullptr;
    }
    std::string callee = canonicalName(expr->callee);
    if (ctx.struct_fields.count(callee) || ctx.struct_fields.count("struct " + callee)) {
        return true;
    }
    return false;
}

std::vector<std::size_t> resolveCall(const Context& ctx,
                                     const ExprPtr& expr,
                                     DebugLoc loc) {
    auto it = ctx.functions_by_name.find(expr->callee);
    if (it == ctx.functions_by_name.end()) {
        if (isAllowedConstructorCall(ctx, expr)) return {};
        failAt("s2validate", std::move(loc),
               "Unknown function call '" + expr->callee + "'");
    }
    std::vector<std::size_t> arity_matches;
    std::vector<std::size_t> exact_matches;
    for (auto id : it->second) {
        const auto& fn = *ctx.functions[id].fn;
        if (fn.params.size() != expr->args.size()) continue;
        arity_matches.push_back(id);
        bool exact = true;
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (!expr->args[i] || !sameTypeShallow(fn.params[i].type, expr->args[i]->type)) {
                exact = false;
                break;
            }
        }
        if (exact) exact_matches.push_back(id);
    }
    if (arity_matches.empty()) {
        failAt("s2validate", std::move(loc),
               "Argument count mismatch for call '" + expr->callee +
                   "': got " + std::to_string(expr->args.size()));
    }
    if (arity_matches.size() == 1) return arity_matches;
    if (exact_matches.size() == 1) return exact_matches;
    failAt("s2validate", std::move(loc),
           "Ambiguous overloaded call '" + expr->callee + "'");
}

void validateExpr(Context& ctx,
                  const ExprPtr& expr,
                  std::vector<Scope>& scopes,
                  std::size_t current_fn);

void validateExprList(Context& ctx,
                      const std::vector<ExprPtr>& exprs,
                      std::vector<Scope>& scopes,
                      std::size_t current_fn) {
    for (const auto& expr : exprs) validateExpr(ctx, expr, scopes, current_fn);
}

void validateExpr(Context& ctx,
                  const ExprPtr& expr,
                  std::vector<Scope>& scopes,
                  std::size_t current_fn) {
    if (!expr) return;
    ErrorContextGuard guard("s2validate", expr->debug_loc, "validating expression");
    switch (expr->kind) {
    case ExprKind::Literal:
        return;
    case ExprKind::VarRef:
        if (!lookupSymbol(scopes, expr->var_name)) {
            failAt("s2validate", expr->debug_loc,
                   "Use of undeclared variable '" + expr->var_name + "'");
        }
        return;
    case ExprKind::Call: {
        validateExprList(ctx, expr->args, scopes, current_fn);
        auto callees = resolveCall(ctx, expr, expr->debug_loc);
        for (auto callee : callees) {
            if (callee != current_fn) ctx.call_graph[current_fn].insert(callee);
            else ctx.call_graph[current_fn].insert(callee);
        }
        return;
    }
    case ExprKind::BinaryOp:
        if (expr->op == ",") {
            failAt("s2validate", expr->debug_loc, "Comma expression is not supported");
        }
        validateExpr(ctx, expr->left, scopes, current_fn);
        validateExpr(ctx, expr->right, scopes, current_fn);
        return;
    case ExprKind::UnaryOp:
        validateExpr(ctx, expr->operand, scopes, current_fn);
        return;
    case ExprKind::ArrayAccess:
        validateExpr(ctx, expr->array_base, scopes, current_fn);
        validateExpr(ctx, expr->index, scopes, current_fn);
        return;
    case ExprKind::FieldAccess:
        validateExpr(ctx, expr->struct_base, scopes, current_fn);
        return;
    case ExprKind::Cast:
        validateExpr(ctx, expr->cast_expr, scopes, current_fn);
        return;
    case ExprKind::Ternary:
        validateExpr(ctx, expr->cond, scopes, current_fn);
        validateExpr(ctx, expr->then_expr, scopes, current_fn);
        validateExpr(ctx, expr->else_expr, scopes, current_fn);
        return;
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc:
    case ExprKind::Slice:
    case ExprKind::BitSelect:
        validateExpr(ctx, expr->base, scopes, current_fn);
        return;
    case ExprKind::Repeat:
        validateExpr(ctx, expr->operand, scopes, current_fn);
        return;
    case ExprKind::WriteSlice:
    case ExprKind::WriteBit:
    case ExprKind::DynamicWriteSlice:
    case ExprKind::DynamicWriteBit:
        validateExpr(ctx, expr->base, scopes, current_fn);
        validateExpr(ctx, expr->index, scopes, current_fn);
        validateExpr(ctx, expr->value, scopes, current_fn);
        return;
    case ExprKind::Concat:
        validateExprList(ctx, expr->parts, scopes, current_fn);
        return;
    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor:
        validateExpr(ctx, expr->operand, scopes, current_fn);
        return;
    }
}

void validateStmtList(Context& ctx,
                      const std::vector<StmtPtr>& stmts,
                      std::vector<Scope>& scopes,
                      std::size_t current_fn,
                      int loop_depth,
                      int switch_depth);

void validateStmt(Context& ctx,
                  const StmtPtr& stmt,
                  std::vector<Scope>& scopes,
                  std::size_t current_fn,
                  int loop_depth,
                  int switch_depth) {
    if (!stmt) return;
    ErrorContextGuard guard("s2validate", stmt->debug_loc, "validating statement");
    std::unordered_set<std::string> visiting;
    switch (stmt->kind) {
    case StmtKind::Decl: {
        validateExprList(ctx, stmt->decl_init_args, scopes, current_fn);
        if (stmt->decl_init) validateExpr(ctx, stmt->decl_init.value(), scopes, current_fn);
        if (stmt->decl_type.is_static) {
            const bool lookup_table = stmt->decl_type.is_array &&
                !stmt->decl_type.init_values.empty();
            if (!lookup_table) {
                failAt("s2validate", stmt->debug_loc,
                       "Static local declaration is not supported unless it is a lookup table");
            }
        }
        validateType(ctx, stmt->decl_type, true, true, stmt->debug_loc, visiting);
        if (stmt->decl_type.is_reference && !stmt->decl_init.has_value()) {
            failAt("s2validate", stmt->debug_loc,
                   "Local reference alias must have an initializer: '" +
                       stmt->decl_name + "'");
        }
        declareSymbol(scopes, stmt->decl_name, stmt->decl_type, stmt->debug_loc);
        return;
    }
    case StmtKind::Assign:
        validateExpr(ctx, stmt->assign_target, scopes, current_fn);
        validateExpr(ctx, stmt->assign_value, scopes, current_fn);
        return;
    case StmtKind::If:
        validateExpr(ctx, stmt->if_cond, scopes, current_fn);
        scopes.push_back({});
        validateStmtList(ctx, stmt->if_then, scopes, current_fn, loop_depth, switch_depth);
        scopes.pop_back();
        scopes.push_back({});
        validateStmtList(ctx, stmt->if_else, scopes, current_fn, loop_depth, switch_depth);
        scopes.pop_back();
        return;
    case StmtKind::For:
        scopes.push_back({});
        if (stmt->for_init) validateStmt(ctx, stmt->for_init, scopes, current_fn, loop_depth, switch_depth);
        validateExpr(ctx, stmt->for_cond, scopes, current_fn);
        validateExpr(ctx, stmt->for_step, scopes, current_fn);
        validateStmtList(ctx, stmt->for_body, scopes, current_fn, loop_depth + 1, switch_depth);
        scopes.pop_back();
        return;
    case StmtKind::While:
    case StmtKind::DoWhile:
        validateExpr(ctx, stmt->while_cond, scopes, current_fn);
        scopes.push_back({});
        validateStmtList(ctx, stmt->while_body, scopes, current_fn, loop_depth + 1, switch_depth);
        scopes.pop_back();
        return;
    case StmtKind::Switch:
        validateExpr(ctx, stmt->switch_expr, scopes, current_fn);
        for (const auto& clause : stmt->switch_cases) {
            if (clause.value) validateExpr(ctx, clause.value.value(), scopes, current_fn);
            scopes.push_back({});
            validateStmtList(ctx, clause.body, scopes, current_fn, loop_depth, switch_depth + 1);
            scopes.pop_back();
        }
        return;
    case StmtKind::Block:
        scopes.push_back({});
        validateStmtList(ctx, stmt->block_stmts, scopes, current_fn, loop_depth, switch_depth);
        scopes.pop_back();
        return;
    case StmtKind::Break:
        if (loop_depth <= 0 && switch_depth <= 0) {
            failAt("s2validate", stmt->debug_loc,
                   "break is only valid inside loop or switch");
        }
        return;
    case StmtKind::Continue:
        if (loop_depth <= 0) {
            failAt("s2validate", stmt->debug_loc,
                   "continue is only valid inside loop");
        }
        return;
    case StmtKind::Return: {
        if (stmt->return_value) validateExpr(ctx, stmt->return_value.value(), scopes, current_fn);
        const auto& view = ctx.functions[current_fn];
        if (view.kind == FunctionKind::Top && stmt->return_value.has_value()) {
            failAt("s2validate", stmt->debug_loc,
                   "Top function may only use void return");
        }
        return;
    }
    case StmtKind::ExprStmt:
        validateExpr(ctx, stmt->expr_stmt, scopes, current_fn);
        return;
    }
}

void validateStmtList(Context& ctx,
                      const std::vector<StmtPtr>& stmts,
                      std::vector<Scope>& scopes,
                      std::size_t current_fn,
                      int loop_depth,
                      int switch_depth) {
    for (const auto& stmt : stmts) {
        validateStmt(ctx, stmt, scopes, current_fn, loop_depth, switch_depth);
    }
}

void collectFunctions(Context& ctx) {
    ctx.functions.push_back(FunctionView{&ctx.top, FunctionKind::Top, ctx.top.name, 0});
    for (const auto& helper : ctx.top.helpers) {
        if (!helper) continue;
        ctx.functions.push_back(FunctionView{helper.get(), FunctionKind::Helper,
                                             helper->name, ctx.functions.size()});
    }
    for (const auto& [name, lambda] : ctx.top.lambdas) {
        if (!lambda) continue;
        ctx.functions.push_back(FunctionView{lambda.get(), FunctionKind::Lambda,
                                             name, ctx.functions.size()});
    }
    for (const auto& view : ctx.functions) {
        ctx.functions_by_name[view.name].push_back(view.id);
    }
}

void validateFunctionSignatures(Context& ctx) {
    std::unordered_set<std::string> exact_signatures;
    for (const auto& view : ctx.functions) {
        ErrorContextGuard guard("s2validate", DebugLoc{}, "validating " + kindName(view.kind) + " '" + view.name + "'");
        const FunctionAST& fn = *view.fn;
        std::unordered_set<std::string> visiting;
        if (view.kind == FunctionKind::Top) {
            if (!isVoidType(fn.return_type)) {
                failAt("s2validate", DebugLoc{}, "Top function must return void");
            }
        } else {
            validateType(ctx, fn.return_type, false, true, DebugLoc{}, visiting);
        }
        auto sig = signatureKey(fn);
        if (!exact_signatures.insert(sig).second) {
            failAt("s2validate", DebugLoc{},
                   "Duplicate function signature is not supported: '" + sig + "'");
        }
        std::unordered_set<std::string> param_names;
        for (const auto& param : fn.params) {
            if (!param_names.insert(param.name).second) {
                failAt("s2validate", param.debug_loc,
                       "Duplicate parameter name in " + kindName(view.kind) +
                           " '" + fn.name + "': '" + param.name + "'");
            }
            if (view.kind == FunctionKind::Top) {
                if (param.passing == ParamPassingKind::Pointer ||
                    param.passing == ParamPassingKind::RValueRef) {
                    failAt("s2validate", param.debug_loc,
                           "Unsupported top-level parameter passing for '" +
                               param.name + "'");
                }
                if (param.passing == ParamPassingKind::MutableRef &&
                    param.direction != ParamDirection::Output) {
                    failAt("s2validate", param.debug_loc,
                           "Mutable reference top-level parameter must be an output: '" +
                               param.name + "'");
                }
                validateTopPortType(ctx, param);
            } else {
                if (param.passing == ParamPassingKind::Pointer ||
                    param.passing == ParamPassingKind::RValueRef) {
                    failAt("s2validate", param.debug_loc,
                           "Unsupported helper/lambda parameter passing for '" +
                               param.name + "'");
                }
                validateType(ctx, param.type, true, true, param.debug_loc, visiting);
            }
        }
    }
}

void validateBodies(Context& ctx) {
    for (const auto& view : ctx.functions) {
        std::vector<Scope> scopes;
        scopes.push_back({});
        for (const auto& param : view.fn->params) {
            declareSymbol(scopes, param.name, param.type, param.debug_loc);
        }
        validateStmtList(ctx, view.fn->body, scopes, view.id, 0, 0);
    }
}

void validateNoLegacyProxyMetadata(const Context& ctx) {
    for (const auto& [name, fields] : ctx.struct_fields) {
        (void)fields;
        TypeInfo type;
        type.name = name;
        type.struct_name = name;
        if (isProxyCarrierType(type)) {
            failAt("s2validate", DebugLoc{},
                   "Unsupported legacy proxy carrier type '" + name + "'");
        }
    }
}

void detectRecursionFrom(const Context& ctx,
                         std::size_t id,
                         std::vector<int>& state,
                         std::vector<std::size_t>& stack) {
    state[id] = 1;
    stack.push_back(id);
    auto it = ctx.call_graph.find(id);
    if (it != ctx.call_graph.end()) {
        for (auto next : it->second) {
            if (ctx.functions[next].kind == FunctionKind::Top) continue;
            if (state[next] == 0) {
                detectRecursionFrom(ctx, next, state, stack);
            } else if (state[next] == 1) {
                std::ostringstream os;
                os << "Recursive helper/lambda call graph detected:";
                auto pos = std::find(stack.begin(), stack.end(), next);
                for (; pos != stack.end(); ++pos) {
                    os << " " << ctx.functions[*pos].name << " ->";
                }
                os << " " << ctx.functions[next].name;
                failAt("s2validate", DebugLoc{}, os.str());
            }
        }
    }
    stack.pop_back();
    state[id] = 2;
}

void validateNoRecursion(Context& ctx) {
    std::vector<int> state(ctx.functions.size(), 0);
    std::vector<std::size_t> stack;
    for (const auto& view : ctx.functions) {
        if (view.kind == FunctionKind::Top) continue;
        if (state[view.id] == 0) detectRecursionFrom(ctx, view.id, state, stack);
    }
}

std::string debugPrint(const Context& ctx) {
    std::ostringstream os;
    os << "s2validate\n";
    os << "top " << ctx.top.name << "\n";
    os << "top_params\n";
    for (const auto& param : ctx.top.params) {
        os << "  " << paramDirectionName(param.direction) << " "
           << typeLabel(param.type) << " " << param.name << "\n";
    }
    os << "functions\n";
    for (const auto& view : ctx.functions) {
        os << "  " << kindName(view.kind) << " " << view.name
           << "/" << view.fn->params.size() << "\n";
    }
    os << "structs\n";
    std::unordered_set<std::string> printed;
    for (const auto& [name, fields] : ctx.struct_fields) {
        auto key = canonicalName(name);
        if (key.empty() || !printed.insert(key).second) continue;
        os << "  " << key << " fields=" << fields.size() << "\n";
    }
    os << "call_graph\n";
    for (const auto& [from, tos] : ctx.call_graph) {
        for (auto to : tos) {
            os << "  " << ctx.functions[from].name << " -> "
               << ctx.functions[to].name << "\n";
        }
    }
    return os.str();
}

ExprKind legacyHardwareKind(s1apinorm::S1HardwareOp op) {
    using s1apinorm::S1HardwareOp;
    switch (op) {
    case S1HardwareOp::ZExt: return ExprKind::ZExt;
    case S1HardwareOp::SExt: return ExprKind::SExt;
    case S1HardwareOp::Trunc: return ExprKind::Trunc;
    case S1HardwareOp::Slice: return ExprKind::Slice;
    case S1HardwareOp::BitSelect: return ExprKind::BitSelect;
    case S1HardwareOp::DynamicSlice: return ExprKind::DynamicWriteSlice;
    case S1HardwareOp::DynamicBitSelect: return ExprKind::DynamicWriteBit;
    case S1HardwareOp::WriteSlice: return ExprKind::WriteSlice;
    case S1HardwareOp::WriteBit: return ExprKind::WriteBit;
    case S1HardwareOp::DynamicWriteSlice: return ExprKind::DynamicWriteSlice;
    case S1HardwareOp::DynamicWriteBit: return ExprKind::DynamicWriteBit;
    case S1HardwareOp::Concat: return ExprKind::Concat;
    case S1HardwareOp::Repeat: return ExprKind::Repeat;
    case S1HardwareOp::ReduceOr: return ExprKind::ReduceOr;
    case S1HardwareOp::ReduceAnd: return ExprKind::ReduceAnd;
    case S1HardwareOp::ReduceXor: return ExprKind::ReduceXor;
    }
    return ExprKind::Call;
}

ExprPtr legacyExprView(const s1apinorm::S1ExprPtr& expr) {
    if (!expr) return nullptr;
    auto out = std::make_shared<Expr>();
    out->type = expr->type;
    out->debug_loc = expr->debug_loc;
    switch (expr->kind) {
    case s1apinorm::S1ExprKind::Literal:
        out->kind = ExprKind::Literal;
        out->literal_value = expr->literal_value;
        return out;
    case s1apinorm::S1ExprKind::VarRef:
        out->kind = ExprKind::VarRef;
        out->var_name = expr->var_name;
        return out;
    case s1apinorm::S1ExprKind::BinaryOp:
        out->kind = ExprKind::BinaryOp;
        out->op = expr->op;
        out->left = legacyExprView(expr->left);
        out->right = legacyExprView(expr->right);
        return out;
    case s1apinorm::S1ExprKind::UnaryOp:
        out->kind = ExprKind::UnaryOp;
        out->op = expr->op;
        out->operand = legacyExprView(expr->operand);
        return out;
    case s1apinorm::S1ExprKind::ArrayAccess:
        out->kind = ExprKind::ArrayAccess;
        out->array_base = legacyExprView(expr->array_base);
        out->index = legacyExprView(expr->index);
        return out;
    case s1apinorm::S1ExprKind::FieldAccess:
        out->kind = ExprKind::FieldAccess;
        out->struct_base = legacyExprView(expr->struct_base);
        out->field_name = expr->field_name;
        return out;
    case s1apinorm::S1ExprKind::Call:
        out->kind = ExprKind::Call;
        out->callee = expr->callee;
        for (const auto& arg : expr->args) out->args.push_back(legacyExprView(arg));
        return out;
    case s1apinorm::S1ExprKind::Cast:
        out->kind = ExprKind::Cast;
        out->cast_type = expr->cast_type;
        out->cast_expr = legacyExprView(expr->cast_expr);
        return out;
    case s1apinorm::S1ExprKind::Ternary:
        out->kind = ExprKind::Ternary;
        out->cond = legacyExprView(expr->cond);
        out->then_expr = legacyExprView(expr->then_expr);
        out->else_expr = legacyExprView(expr->else_expr);
        return out;
    case s1apinorm::S1ExprKind::HardwareOp:
        out->kind = legacyHardwareKind(expr->hardware_op);
        out->base = legacyExprView(expr->base);
        out->value = legacyExprView(expr->value);
        out->index = legacyExprView(expr->index);
        out->operand = legacyExprView(expr->operand);
        out->cast_expr = legacyExprView(expr->cast_expr);
        for (const auto& part : expr->parts) out->parts.push_back(legacyExprView(part));
        out->hi = expr->hi;
        out->lo = expr->lo;
        out->bit = expr->bit;
        out->times = expr->times;
        out->to_width = expr->to_width;
        return out;
    }
    return out;
}

StmtPtr legacyStmtView(const s1apinorm::S1StmtPtr& stmt);

std::vector<StmtPtr> legacyStmtListView(const std::vector<s1apinorm::S1StmtPtr>& stmts) {
    std::vector<StmtPtr> out;
    out.reserve(stmts.size());
    for (const auto& stmt : stmts) out.push_back(legacyStmtView(stmt));
    return out;
}

std::vector<CaseClause> legacyCaseView(const std::vector<s1apinorm::S1CaseClause>& cases) {
    std::vector<CaseClause> out;
    out.reserve(cases.size());
    for (const auto& one : cases) {
        CaseClause c;
        if (one.value) c.value = legacyExprView(one.value.value());
        c.body = legacyStmtListView(one.body);
        out.push_back(std::move(c));
    }
    return out;
}

StmtPtr legacyStmtView(const s1apinorm::S1StmtPtr& stmt) {
    if (!stmt) return nullptr;
    auto out = std::make_shared<Stmt>();
    out->debug_loc = stmt->debug_loc;
    switch (stmt->kind) {
    case s1apinorm::S1StmtKind::Decl:
        out->kind = StmtKind::Decl;
        out->decl_type = stmt->decl_type;
        out->decl_name = stmt->decl_name;
        out->decl_default_constructed = stmt->decl_default_constructed;
        break;
    case s1apinorm::S1StmtKind::Assign:
        out->kind = StmtKind::Assign;
        out->assign_target = legacyExprView(stmt->assign_target);
        out->assign_value = legacyExprView(stmt->assign_value);
        break;
    case s1apinorm::S1StmtKind::Construct: {
        out->kind = StmtKind::Assign;
        out->assign_target = legacyExprView(stmt->construct_target);
        auto call = std::make_shared<Expr>();
        call->kind = ExprKind::Call;
        call->debug_loc = stmt->debug_loc;
        call->type = stmt->construct_type;
        call->callee = stmt->construct_callee;
        for (const auto& arg : stmt->construct_args) call->args.push_back(legacyExprView(arg));
        out->assign_value = std::move(call);
        break;
    }
    case s1apinorm::S1StmtKind::If:
        out->kind = StmtKind::If;
        out->if_cond = legacyExprView(stmt->if_cond);
        out->if_then = legacyStmtListView(stmt->if_then);
        out->if_else = legacyStmtListView(stmt->if_else);
        break;
    case s1apinorm::S1StmtKind::For:
        out->kind = StmtKind::For;
        if (!stmt->for_init.empty()) {
            out->for_init = legacyStmtView(stmt->for_init.front());
        }
        out->for_cond = legacyExprView(stmt->for_cond);
        out->for_step = legacyExprView(stmt->for_step);
        out->for_body = legacyStmtListView(stmt->for_body);
        for (auto it = stmt->for_init.rbegin();
             it != stmt->for_init.rend() && std::next(it) != stmt->for_init.rend();
             ++it) {
            out->for_body.insert(out->for_body.begin(), legacyStmtView(*it));
        }
        break;
    case s1apinorm::S1StmtKind::While:
        out->kind = StmtKind::While;
        out->while_cond = legacyExprView(stmt->while_cond);
        out->while_body = legacyStmtListView(stmt->while_body);
        break;
    case s1apinorm::S1StmtKind::DoWhile:
        out->kind = StmtKind::DoWhile;
        out->while_cond = legacyExprView(stmt->while_cond);
        out->while_body = legacyStmtListView(stmt->while_body);
        break;
    case s1apinorm::S1StmtKind::Switch:
        out->kind = StmtKind::Switch;
        out->switch_expr = legacyExprView(stmt->switch_expr);
        out->switch_cases = legacyCaseView(stmt->switch_cases);
        break;
    case s1apinorm::S1StmtKind::Block:
        out->kind = StmtKind::Block;
        out->block_stmts = legacyStmtListView(stmt->block_stmts);
        break;
    case s1apinorm::S1StmtKind::Break:
        out->kind = StmtKind::Break;
        break;
    case s1apinorm::S1StmtKind::Continue:
        out->kind = StmtKind::Continue;
        break;
    case s1apinorm::S1StmtKind::Return:
        out->kind = StmtKind::Return;
        if (stmt->return_value) out->return_value = legacyExprView(stmt->return_value.value());
        break;
    case s1apinorm::S1StmtKind::ExprStmt:
        out->kind = StmtKind::ExprStmt;
        out->expr_stmt = legacyExprView(stmt->expr_stmt);
        break;
    }
    return out;
}

FunctionAST legacyFunctionView(const s1apinorm::S1FunctionAST& function) {
    FunctionAST out;
    out.name = function.name;
    out.return_type = function.return_type;
    out.params = function.params;
    out.body = legacyStmtListView(function.body);
    out.struct_fields = function.struct_fields;
    out.struct_constructors = function.struct_constructors;
    for (const auto& helper : function.helpers) {
        if (!helper) continue;
        out.helpers.push_back(std::make_shared<FunctionAST>(legacyFunctionView(*helper)));
    }
    for (const auto& [name, lambda] : function.lambdas) {
        if (!lambda) continue;
        out.lambdas[name] = std::make_shared<FunctionAST>(legacyFunctionView(*lambda));
    }
    return out;
}

ValidateResult runValidate(const FunctionAST& function,
                           const ValidateOptions& options) {
    Context ctx{function, options};
    ctx.struct_fields = function.struct_fields;
    collectFunctions(ctx);
    validateNoLegacyProxyMetadata(ctx);
    validateFunctionSignatures(ctx);
    validateBodies(ctx);
    validateNoRecursion(ctx);

    ValidateResult result;
    result.warnings = std::move(ctx.warnings);
    if (options.debug_print) result.debug_text = debugPrint(ctx);
    return result;
}

ValidateResult runValidate(const s1apinorm::S1FunctionAST& function,
                           const ValidateOptions& options) {
    return runValidate(legacyFunctionView(function), options);
}

} // namespace

ValidateResult validateFunctionAST(const FunctionAST& function,
                                   const ValidateOptions& options) {
    try {
        return runValidate(function, options);
    } catch (const RTLZZException& ex) {
        ValidateResult result;
        ValidateError error;
        error.message = ex.message();
        error.formatted = ex.what();
        if (auto context = ex.primaryContext()) error.context = *context;
        result.error = std::move(error);
        return result;
    }
}

ValidateResult validateFunctionAST(const s1apinorm::S1FunctionAST& function,
                                   const ValidateOptions& options) {
    try {
        return runValidate(function, options);
    } catch (const RTLZZException& ex) {
        ValidateResult result;
        ValidateError error;
        error.message = ex.message();
        error.formatted = ex.what();
        if (auto context = ex.primaryContext()) error.context = *context;
        result.error = std::move(error);
        return result;
    }
}

void validateFunctionASTOrThrow(const FunctionAST& function,
                                const ValidateOptions& options) {
    auto result = runValidate(function, options);
    (void)result;
}

void validateFunctionASTOrThrow(const s1apinorm::S1FunctionAST& function,
                                const ValidateOptions& options) {
    auto result = runValidate(function, options);
    (void)result;
}

} // namespace pred::s2validate
