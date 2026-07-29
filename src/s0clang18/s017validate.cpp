#include "s0clang18/s017validate.hpp"

#include "s0clang18/s006type.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pred::s0clang18 {
namespace {

using pred::v2::Expr;
using pred::v2::ExprKind;
using pred::v2::ExprPtr;
using pred::v2::FunctionAST;
using pred::v2::ParamDecl;
using pred::v2::Stmt;
using pred::v2::StmtKind;
using pred::v2::StmtPtr;
using pred::v2::TypeInfo;

const char* issueKindLabel(SurfaceIssueKind kind) {
    switch (kind) {
    case SurfaceIssueKind::UnknownType:
        return "unknown type";
    case SurfaceIssueKind::UnresolvedCallee:
        return "unresolved callee";
    case SurfaceIssueKind::UnresolvedSymbol:
        return "unresolved symbol";
    case SurfaceIssueKind::IllegalPort:
        return "illegal port";
    case SurfaceIssueKind::IllegalReferenceOrPointer:
        return "illegal reference or pointer";
    case SurfaceIssueKind::UnsupportedClangNode:
        return "unsupported clang node";
    case SurfaceIssueKind::UnsupportedCppSubset:
        return "unsupported C++ subset";
    }
    return "surface validation issue";
}

Diagnostic diagnosticForIssue(const SurfaceValidationIssue& issue) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message =
        std::string(issueKindLabel(issue.kind)) + ": " + issue.message;
    diagnostic.context.stage = "s0clang18.17";
    diagnostic.context.loc = issue.loc;
    diagnostic.context.source_file = diagnostic.context.loc.file;
    return diagnostic;
}

void addIssue(SurfaceValidationResult& result,
              SurfaceIssueKind kind,
              DebugLoc loc,
              std::string message) {
    SurfaceValidationIssue issue;
    issue.kind = kind;
    issue.message = std::move(message);
    issue.loc = std::move(loc);
    result.diagnostics.push_back(diagnosticForIssue(issue));
    result.issues.push_back(std::move(issue));
}

std::string canonicalName(std::string name) {
    while (name.rfind("const ", 0) == 0) name.erase(0, 6);
    while (name.rfind("volatile ", 0) == 0) name.erase(0, 9);
    while (name.rfind("struct ", 0) == 0) name.erase(0, 7);
    while (name.rfind("class ", 0) == 0) name.erase(0, 6);
    while (!name.empty() && (name.back() == '&' || name.back() == '*')) {
        name.pop_back();
    }
    return name;
}

std::unordered_set<std::string> recordNames(const RecordMetadataSet& records) {
    std::unordered_set<std::string> out;
    for (const RecordMetadata& record : records.records) {
        if (!record.key.canonical_name.empty()) {
            out.insert(canonicalName(record.key.canonical_name));
        }
        auto found_name = records.record_by_name.find(record.key.canonical_name);
        if (found_name != records.record_by_name.end()) {
            out.insert(canonicalName(found_name->first));
        }
    }
    for (const auto& [name, _] : records.record_by_name) {
        out.insert(canonicalName(name));
    }
    return out;
}

std::unordered_set<std::string> reachableNames(
    const FunctionReachabilityGraph& reachability) {
    std::unordered_set<std::string> out;
    for (const FunctionEntity& function : reachability.functions) {
        if (!function.key.stable_name.empty()) out.insert(function.key.stable_name);
    }
    for (const auto& [name, _] : reachability.function_by_stable_name) {
        if (!name.empty()) out.insert(name);
    }
    return out;
}

std::unordered_set<std::string> portNames(const PortDeclTable& ports) {
    std::unordered_set<std::string> out;
    for (const RawPortDecl& port : ports.ports) {
        if (!port.name.empty()) out.insert(port.name);
    }
    for (const auto& [name, _] : ports.port_by_name) {
        if (!name.empty()) out.insert(name);
    }
    return out;
}

bool isVoidType(const TypeInfo& type) {
    return type.name == "void" && type.width == 0 && !type.is_hw_int &&
           type.struct_name.empty() && !type.is_array && !type.is_pointer &&
           !type.is_reference;
}

bool isLambdaObjectType(const TypeInfo& type) {
    return type.name.find("(lambda") != std::string::npos ||
           type.struct_name.find("(lambda") != std::string::npos;
}

bool isKnownScalarType(const TypeInfo& type,
                       const std::unordered_set<std::string>& records) {
    if (isVoidType(type)) return true;
    if (type.is_hw_int || type.hw_kind == "Int" || type.hw_kind == "bool") {
        return type.width > 0;
    }
    if (!type.struct_name.empty()) {
        return records.count(canonicalName(type.struct_name)) > 0;
    }
    if (!type.name.empty()) {
        if (type.name == "bool") return type.width == 1;
        if (type.name.rfind("Int<", 0) == 0) return type.width > 0;
        return records.count(canonicalName(type.name)) > 0;
    }
    return false;
}

void validateType(SurfaceValidationResult& result,
                  const TypeInfo& type,
                  DebugLoc loc,
                  const std::unordered_set<std::string>& records,
                  bool allow_reference,
                  const std::string& subject) {
    if (type.is_pointer) {
        addIssue(result, SurfaceIssueKind::IllegalReferenceOrPointer, loc,
                 subject + " uses pointer type '" + typeLabel(type) + "'");
    }
    if (type.is_reference && !allow_reference) {
        addIssue(result, SurfaceIssueKind::IllegalReferenceOrPointer, loc,
                 subject + " uses reference type '" + typeLabel(type) + "'");
    }
    if (isLambdaObjectType(type)) return;
    if (type.is_array) {
        if (type.array_dims.empty()) {
            addIssue(result, SurfaceIssueKind::UnknownType, loc,
                     subject + " is an array with no dimensions");
        }
        for (int dim : type.array_dims) {
            if (dim <= 0) {
                addIssue(result, SurfaceIssueKind::UnknownType, loc,
                         subject + " has non-positive array dimension");
            }
        }
    }
    if (!isKnownScalarType(type, records)) {
        addIssue(result, SurfaceIssueKind::UnknownType, loc,
                 subject + " has unknown type '" + typeLabel(type) + "'");
    }
}

bool isConstructorCallee(const Expr& expr,
                         const std::unordered_set<std::string>& records) {
    const std::string callee = canonicalName(expr.callee);
    if (callee == "Int" || callee == "bool" || callee.rfind("Int<", 0) == 0) {
        return true;
    }
    if (expr.type.is_array && callee == canonicalName(typeLabel(expr.type))) {
        return true;
    }
    if (!expr.type.struct_name.empty() &&
        callee == canonicalName(expr.type.struct_name)) {
        return true;
    }
    if (!expr.type.name.empty() && callee == canonicalName(expr.type.name)) {
        return true;
    }
    return records.count(callee) > 0;
}

bool isSupportedAPICallee(const Expr& expr) {
    const std::string callee = canonicalName(expr.callee);
    if (expr.intrinsic != pred::v2::IntrinsicKind::None) return true;
    return callee == "at" || callee == "_at" || callee == "__slice" ||
           callee == "__bit" || callee == "range_at" || callee == "bit_at" ||
           callee == "pick" || callee == "Cat" || callee == "cat" ||
           callee == "concat" || callee == "Repeat" || callee == "repeat" ||
           callee == "ReduceOr" || callee == "reduce_or" ||
           callee == "ReduceAnd" || callee == "reduce_and" ||
           callee == "ReduceXor" || callee == "reduce_xor" ||
           callee == "zext" || callee == "ZExt" ||
           callee == "sext" || callee == "SExt" ||
           callee == "trunc" || callee == "Trunc" ||
           callee == "to" || callee == "To" ||
           callee == "sint" || callee == "sint_view";
}

struct ValidationContext {
    SurfaceValidationResult& result;
    const std::unordered_set<std::string>& ports;
    const std::unordered_set<std::string>& records;
    const std::unordered_set<std::string>& reachable_functions;
};

void validateExpr(const ValidationContext& context, const ExprPtr& expr);
void validateStmt(const ValidationContext& context, const StmtPtr& stmt);

void requireChild(const ValidationContext& context,
                  const ExprPtr& child,
                  DebugLoc loc,
                  std::string message) {
    if (!child) {
        addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset,
                 std::move(loc), std::move(message));
        return;
    }
    validateExpr(context, child);
}

void validateExprList(const ValidationContext& context,
                      const std::vector<ExprPtr>& exprs) {
    for (const ExprPtr& expr : exprs) {
        if (!expr) {
            addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset, {},
                     "expression list contains a null expression");
            continue;
        }
        validateExpr(context, expr);
    }
}

void validateExpr(const ValidationContext& context, const ExprPtr& expr) {
    if (!expr) {
        addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset, {},
                 "null expression");
        return;
    }

    validateType(context.result, expr->type, expr->debug_loc, context.records,
                 true, "expression");

    if (!expr->global_port_name.empty() &&
        context.ports.count(expr->global_port_name) == 0) {
        addIssue(context.result, SurfaceIssueKind::IllegalPort, expr->debug_loc,
                 "expression references non-port global '" +
                     expr->global_port_name + "'");
    }

    switch (expr->kind) {
    case ExprKind::Literal:
        if (expr->literal_value.empty()) {
            addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset,
                     expr->debug_loc, "literal expression has no value");
        }
        break;
    case ExprKind::VarRef:
        if (expr->var_name.empty()) {
            addIssue(context.result, SurfaceIssueKind::UnresolvedSymbol,
                     expr->debug_loc, "variable reference has no name");
        }
        break;
    case ExprKind::BinaryOp:
        if (expr->op.empty()) {
            addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset,
                     expr->debug_loc, "binary expression has no operator");
        }
        requireChild(context, expr->left, expr->debug_loc,
                     "binary expression is missing lhs");
        requireChild(context, expr->right, expr->debug_loc,
                     "binary expression is missing rhs");
        break;
    case ExprKind::UnaryOp:
        if (expr->op.empty()) {
            addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset,
                     expr->debug_loc, "unary expression has no operator");
        }
        requireChild(context, expr->operand, expr->debug_loc,
                     "unary expression is missing operand");
        break;
    case ExprKind::ArrayAccess:
        requireChild(context, expr->array_base, expr->debug_loc,
                     "array access is missing base");
        requireChild(context, expr->index, expr->debug_loc,
                     "array access is missing index");
        break;
    case ExprKind::FieldAccess:
        requireChild(context, expr->struct_base, expr->debug_loc,
                     "field access is missing base");
        if (expr->field_name.empty()) {
            addIssue(context.result, SurfaceIssueKind::UnresolvedSymbol,
                     expr->debug_loc, "field access has no field name");
        }
        break;
    case ExprKind::Call:
        if (expr->callee.empty()) {
            addIssue(context.result, SurfaceIssueKind::UnresolvedCallee,
                     expr->debug_loc, "call expression has no callee");
        } else if (expr->callee != "__init_list" &&
                   context.reachable_functions.count(expr->callee) == 0 &&
                   !isConstructorCallee(*expr, context.records) &&
                   !isSupportedAPICallee(*expr)) {
            addIssue(context.result, SurfaceIssueKind::UnresolvedCallee,
                     expr->debug_loc,
                     "call callee '" + expr->callee +
                         "' is not a reachable helper, constructor, or supported API");
        }
        validateExprList(context, expr->args);
        break;
    case ExprKind::Cast:
        validateType(context.result, expr->cast_type, expr->debug_loc,
                     context.records, true, "cast target");
        requireChild(context, expr->cast_expr, expr->debug_loc,
                     "cast expression is missing operand");
        break;
    case ExprKind::Ternary:
        requireChild(context, expr->cond, expr->debug_loc,
                     "ternary expression is missing condition");
        requireChild(context, expr->then_expr, expr->debug_loc,
                     "ternary expression is missing true branch");
        requireChild(context, expr->else_expr, expr->debug_loc,
                     "ternary expression is missing false branch");
        break;
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc:
        if (expr->to_width <= 0) {
            addIssue(context.result, SurfaceIssueKind::UnknownType,
                     expr->debug_loc, "width-changing expression has no target width");
        }
        requireChild(context, expr->cast_expr, expr->debug_loc,
                     "width-changing expression is missing operand");
        break;
    case ExprKind::Slice:
        if (expr->hi < expr->lo || expr->lo < 0) {
            addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset,
                     expr->debug_loc, "slice expression has invalid bounds");
        }
        requireChild(context, expr->base, expr->debug_loc,
                     "slice expression is missing base");
        break;
    case ExprKind::BitSelect:
        if (expr->bit < 0) {
            addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset,
                     expr->debug_loc, "bit select expression has invalid bit index");
        }
        requireChild(context, expr->base, expr->debug_loc,
                     "bit select expression is missing base");
        break;
    case ExprKind::WriteSlice:
    case ExprKind::WriteBit:
        requireChild(context, expr->base, expr->debug_loc,
                     "static write expression is missing base");
        requireChild(context, expr->value, expr->debug_loc,
                     "static write expression is missing value");
        break;
    case ExprKind::DynamicWriteSlice:
    case ExprKind::DynamicWriteBit:
        requireChild(context, expr->base, expr->debug_loc,
                     "dynamic write expression is missing base");
        requireChild(context, expr->index, expr->debug_loc,
                     "dynamic write expression is missing index");
        requireChild(context, expr->value, expr->debug_loc,
                     "dynamic write expression is missing value");
        break;
    case ExprKind::Concat:
        if (expr->parts.empty()) {
            addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset,
                     expr->debug_loc, "concat expression has no parts");
        }
        validateExprList(context, expr->parts);
        break;
    case ExprKind::Repeat:
        if (expr->times <= 0) {
            addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset,
                     expr->debug_loc, "repeat expression has non-positive count");
        }
        requireChild(context, expr->operand, expr->debug_loc,
                     "repeat expression is missing operand");
        break;
    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor:
        requireChild(context, expr->operand, expr->debug_loc,
                     "reduce expression is missing operand");
        break;
    }
}

void validateStmtList(const ValidationContext& context,
                      const std::vector<StmtPtr>& stmts) {
    for (const StmtPtr& stmt : stmts) validateStmt(context, stmt);
}

void validateStmt(const ValidationContext& context, const StmtPtr& stmt) {
    if (!stmt) {
        addIssue(context.result, SurfaceIssueKind::UnsupportedCppSubset, {},
                 "null statement");
        return;
    }

    switch (stmt->kind) {
    case StmtKind::Assign:
        requireChild(context, stmt->assign_target, stmt->debug_loc,
                     "assignment is missing target");
        requireChild(context, stmt->assign_value, stmt->debug_loc,
                     "assignment is missing value");
        break;
    case StmtKind::Decl:
        if (stmt->decl_name.empty()) {
            addIssue(context.result, SurfaceIssueKind::UnresolvedSymbol,
                     stmt->debug_loc, "declaration has no name");
        }
        validateType(context.result, stmt->decl_type, stmt->debug_loc,
                     context.records, true, "declaration '" + stmt->decl_name + "'");
        if (stmt->decl_init) validateExpr(context, *stmt->decl_init);
        validateExprList(context, stmt->decl_init_args);
        break;
    case StmtKind::If:
        requireChild(context, stmt->if_cond, stmt->debug_loc,
                     "if statement is missing condition");
        validateStmtList(context, stmt->if_then);
        validateStmtList(context, stmt->if_else);
        break;
    case StmtKind::For:
        if (stmt->for_init) validateStmt(context, stmt->for_init);
        if (stmt->for_cond) validateExpr(context, stmt->for_cond);
        if (stmt->for_step) validateExpr(context, stmt->for_step);
        validateStmtList(context, stmt->for_body);
        break;
    case StmtKind::While:
        requireChild(context, stmt->while_cond, stmt->debug_loc,
                     "while statement is missing condition");
        validateStmtList(context, stmt->while_body);
        break;
    case StmtKind::DoWhile:
        requireChild(context, stmt->while_cond, stmt->debug_loc,
                     "do-while statement is missing condition");
        validateStmtList(context, stmt->while_body);
        break;
    case StmtKind::Switch:
        requireChild(context, stmt->switch_expr, stmt->debug_loc,
                     "switch statement is missing selector");
        for (const pred::v2::CaseClause& clause : stmt->switch_cases) {
            if (clause.value) validateExpr(context, *clause.value);
            validateStmtList(context, clause.body);
        }
        break;
    case StmtKind::Block:
        validateStmtList(context, stmt->block_stmts);
        break;
    case StmtKind::Break:
    case StmtKind::Continue:
        break;
    case StmtKind::Return:
        if (stmt->return_value) validateExpr(context, *stmt->return_value);
        break;
    case StmtKind::ExprStmt:
        if (stmt->expr_stmt) validateExpr(context, stmt->expr_stmt);
        break;
    }
}

void collectFunctions(const FunctionAST& root,
                      std::vector<const FunctionAST*>& functions) {
    functions.push_back(&root);
    for (const auto& helper : root.helpers) {
        if (helper) collectFunctions(*helper, functions);
    }
    for (const auto& [_, lambda] : root.lambdas) {
        if (lambda) collectFunctions(*lambda, functions);
    }
}

void validateRecordMetadata(SurfaceValidationResult& result,
                            const RecordMetadataSet& records,
                            const std::unordered_set<std::string>& record_names) {
    for (const RecordMetadata& record : records.records) {
        if (record.key.canonical_name.empty()) {
            addIssue(result, SurfaceIssueKind::UnknownType, record.loc,
                     "record metadata has no canonical name");
        }
        for (const RecordField& field : record.fields) {
            if (field.name.empty()) {
                addIssue(result, SurfaceIssueKind::UnresolvedSymbol, field.loc,
                         "record field has no name");
            }
            validateType(result, field.type, field.loc, record_names, false,
                         "record field '" + field.name + "'");
        }
    }
}

void validatePorts(SurfaceValidationResult& result,
                   const PortDeclTable& ports,
                   const std::unordered_set<std::string>& record_names) {
    for (std::size_t i = 0; i < ports.ports.size(); ++i) {
        const RawPortDecl& port = ports.ports[i];
        if (port.name.empty()) {
            addIssue(result, SurfaceIssueKind::IllegalPort, port.decl_loc,
                     "port declaration has no name");
        }
        auto found = ports.port_by_name.find(port.name);
        if (found == ports.port_by_name.end() || found->second != i) {
            addIssue(result, SurfaceIssueKind::IllegalPort, port.decl_loc,
                     "port '" + port.name + "' is missing from port index");
        }
        validateType(result, port.type, port.decl_loc, record_names, true,
                     "port '" + port.name + "'");
    }
}

void validateFunction(SurfaceValidationResult& result,
                      const FunctionAST& function,
                      const ValidationContext& context) {
    DebugLoc function_loc;
    if (!function.params.empty()) function_loc = function.params.front().debug_loc;
    if (function.name.empty()) {
        addIssue(result, SurfaceIssueKind::UnresolvedSymbol, function_loc,
                 "function has no stable name");
    } else if (context.reachable_functions.count(function.name) == 0) {
        addIssue(result, SurfaceIssueKind::UnresolvedSymbol, function_loc,
                 "function '" + function.name +
                     "' is not present in the reachability graph");
    }

    validateType(result, function.return_type, function_loc, context.records,
                 true, "function '" + function.name + "' return type");
    for (const ParamDecl& param : function.params) {
        if (param.name.empty()) {
            addIssue(result, SurfaceIssueKind::UnresolvedSymbol, param.debug_loc,
                     "function '" + function.name + "' has unnamed parameter");
        }
        validateType(result, param.type, param.debug_loc, context.records,
                     true, "parameter '" + param.name + "'");
    }
    validateStmtList(context, function.body);
}

void validateReachabilityCoverage(SurfaceValidationResult& result,
                                  const std::vector<const FunctionAST*>& functions,
                                  const FunctionReachabilityGraph& reachability) {
    std::unordered_set<std::string> ast_names;
    for (const FunctionAST* function : functions) {
        if (function && !function->name.empty()) ast_names.insert(function->name);
    }
    for (const FunctionEntity& entity : reachability.functions) {
        if (entity.key.stable_name.empty()) {
            addIssue(result, SurfaceIssueKind::UnresolvedSymbol, entity.loc,
                     "reachable function has no stable name");
            continue;
        }
        if (ast_names.count(entity.key.stable_name) == 0) {
            addIssue(result, SurfaceIssueKind::UnresolvedSymbol, entity.loc,
                     "reachable function '" + entity.key.stable_name +
                         "' has no surface FunctionAST");
        }
    }
}

} // namespace

SurfaceValidationResult validateSurfaceAST(
    const pred::v2::FunctionAST& function,
    const PortDeclTable& ports,
    const RecordMetadataSet& records,
    const FunctionReachabilityGraph& reachability) {
    SurfaceValidationResult result;
    const auto known_ports = portNames(ports);
    const auto known_records = recordNames(records);
    const auto known_functions = reachableNames(reachability);

    validateRecordMetadata(result, records, known_records);
    validatePorts(result, ports, known_records);

    ValidationContext context{result, known_ports, known_records, known_functions};
    std::vector<const FunctionAST*> functions;
    collectFunctions(function, functions);
    validateReachabilityCoverage(result, functions, reachability);
    for (const FunctionAST* ast_function : functions) {
        if (ast_function) validateFunction(result, *ast_function, context);
    }
    return result;
}

} // namespace pred::s0clang18
