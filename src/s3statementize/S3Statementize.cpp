#include "s3statementize/S3Statementize.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace pred::s3statementize {
namespace {

struct LowerContext {
    std::string function_name;
    std::unordered_set<std::string> used_names;
    std::unordered_set<std::string> struct_names;
    int temp_counter = 0;
};

std::string sanitizeName(std::string name) {
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    }
    if (name.empty()) name = "expr";
    return name;
}

TypeInfo unknownType() {
    TypeInfo type;
    type.name = "unknown";
    return type;
}

TypeInfo storageType(TypeInfo type) {
    type.is_reference = false;
    type.is_pointer = false;
    type.is_const = false;
    type.is_mutable = true;
    return canonicalize_bool_type(std::move(type));
}

std::string canonicalName(std::string name) {
    if (name.rfind("struct ", 0) == 0) name = name.substr(7);
    if (name.rfind("class ", 0) == 0) name = name.substr(6);
    auto lt = name.find('<');
    if (lt != std::string::npos) name = name.substr(0, lt);
    return name;
}

std::string makeTempName(LowerContext& ctx, const std::string& hint) {
    std::string base = "__tmp_" + sanitizeName(ctx.function_name) + "_" +
                       sanitizeName(hint) + "_";
    while (true) {
        std::string candidate = base + std::to_string(ctx.temp_counter++);
        if (!ctx.used_names.count(candidate)) {
            ctx.used_names.insert(candidate);
            return candidate;
        }
    }
}

S3StmtPtr makeDecl(const std::string& name, TypeInfo type, DebugLoc loc) {
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Decl;
    stmt->debug_loc = std::move(loc);
    stmt->decl_name = name;
    stmt->decl_type = storageType(std::move(type));
    return stmt;
}

Operand varOperand(const std::string& name, TypeInfo type, DebugLoc loc = {}) {
    Operand out;
    out.kind = OperandKind::Var;
    out.var_name = name;
    out.type = canonicalize_bool_type(std::move(type));
    out.debug_loc = std::move(loc);
    return out;
}

Operand literalOperand(const std::string& value, TypeInfo type, DebugLoc loc = {}) {
    Operand out;
    out.kind = OperandKind::Literal;
    out.literal_value = value;
    out.type = canonicalize_bool_type(std::move(type));
    out.debug_loc = std::move(loc);
    return out;
}

Operand lvalueOperand(LValue lvalue) {
    Operand out;
    out.kind = OperandKind::LValueRead;
    out.type = lvalue.type;
    out.debug_loc = lvalue.debug_loc;
    out.lvalue = std::move(lvalue);
    return out;
}

LValue varLValue(const std::string& name, TypeInfo type, DebugLoc loc = {}) {
    LValue out;
    out.root = name;
    out.type = canonicalize_bool_type(std::move(type));
    out.debug_loc = std::move(loc);
    return out;
}

S3StmtPtr makeAssign(LValue target, Operand value, DebugLoc loc) {
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Assign;
    stmt->debug_loc = std::move(loc);
    stmt->target = std::move(target);
    stmt->value = std::move(value);
    return stmt;
}

S3StmtPtr makeOp(LValue target, OpExpr op, DebugLoc loc) {
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Op;
    stmt->debug_loc = std::move(loc);
    stmt->target = std::move(target);
    stmt->op = std::move(op);
    return stmt;
}

S3StmtPtr makeCall(std::optional<LValue> target,
                   const std::string& callee,
                   std::vector<Operand> args,
                   TypeInfo result_type,
                   DebugLoc loc) {
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Call;
    stmt->debug_loc = std::move(loc);
    stmt->call_result = std::move(target);
    stmt->callee = callee;
    stmt->args = std::move(args);
    stmt->result_type = canonicalize_bool_type(std::move(result_type));
    return stmt;
}

S3StmtPtr makeConstruct(LValue target,
                        const std::string& callee,
                        std::vector<Operand> args,
                        TypeInfo result_type,
                        DebugLoc loc) {
    auto stmt = std::make_shared<S3Stmt>();
    stmt->kind = S3StmtKind::Construct;
    stmt->debug_loc = std::move(loc);
    stmt->target = std::move(target);
    stmt->callee = callee;
    stmt->args = std::move(args);
    stmt->result_type = canonicalize_bool_type(std::move(result_type));
    return stmt;
}

[[noreturn]] void fail(DebugLoc loc, const std::string& message) {
    ErrorContext context;
    context.stage = "s3statementize";
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    throwRTLZZ(std::move(context), message);
}

UnaryOp parseUnaryOp(const std::string& op, DebugLoc loc) {
    if (op == "!") return UnaryOp::LogicalNot;
    if (op == "~") return UnaryOp::BitNot;
    if (op == "-") return UnaryOp::Negate;
    if (op == "+") return UnaryOp::Plus;
    fail(std::move(loc), "Unsupported unary operator '" + op + "'");
}

BinaryOp parseBinaryOp(const std::string& op, DebugLoc loc) {
    if (op == "+") return BinaryOp::Add;
    if (op == "-") return BinaryOp::Sub;
    if (op == "*") return BinaryOp::Mul;
    if (op == "/") return BinaryOp::Div;
    if (op == "%") return BinaryOp::Mod;
    if (op == "<<") return BinaryOp::Shl;
    if (op == ">>") return BinaryOp::Shr;
    if (op == "&") return BinaryOp::BitAnd;
    if (op == "|") return BinaryOp::BitOr;
    if (op == "^") return BinaryOp::BitXor;
    if (op == "&&") return BinaryOp::LogicalAnd;
    if (op == "||") return BinaryOp::LogicalOr;
    if (op == "==") return BinaryOp::Eq;
    if (op == "!=") return BinaryOp::Ne;
    if (op == "<") return BinaryOp::Lt;
    if (op == "<=") return BinaryOp::Le;
    if (op == ">") return BinaryOp::Gt;
    if (op == ">=") return BinaryOp::Ge;
    fail(std::move(loc), "Unsupported binary operator '" + op + "'");
}

bool isHardwareConstructorName(const std::string& callee) {
    return callee == "Int" || callee == "UInt" || callee == "bool" ||
           callee.rfind("Int<", 0) == 0 || callee.rfind("UInt<", 0) == 0;
}

bool isConstructorCall(const ExprPtr& expr, const LowerContext& ctx) {
    if (!expr || expr->kind != ExprKind::Call) return false;
    if (isHardwareConstructorName(expr->callee)) {
        return true;
    }
    std::string callee = canonicalName(expr->callee);
    if (!expr->type.struct_name.empty() &&
        callee == canonicalName(expr->type.struct_name)) {
        return true;
    }
    if (!expr->type.name.empty() && callee == canonicalName(expr->type.name) &&
        ctx.struct_names.count(callee)) {
        return true;
    }
    if (ctx.struct_names.count(callee)) return true;
    return false;
}

void collectNamesExpr(const ExprPtr& expr, std::unordered_set<std::string>& out);

void collectNamesStmt(const StmtPtr& stmt, std::unordered_set<std::string>& out) {
    if (!stmt) return;
    if (stmt->kind == StmtKind::Decl && !stmt->decl_name.empty()) out.insert(stmt->decl_name);
    collectNamesExpr(stmt->assign_target, out);
    collectNamesExpr(stmt->assign_value, out);
    if (stmt->decl_init) collectNamesExpr(stmt->decl_init.value(), out);
    for (const auto& arg : stmt->decl_init_args) collectNamesExpr(arg, out);
    collectNamesExpr(stmt->if_cond, out);
    for (const auto& child : stmt->if_then) collectNamesStmt(child, out);
    for (const auto& child : stmt->if_else) collectNamesStmt(child, out);
    if (stmt->for_init) collectNamesStmt(stmt->for_init, out);
    collectNamesExpr(stmt->for_cond, out);
    collectNamesExpr(stmt->for_step, out);
    for (const auto& child : stmt->for_body) collectNamesStmt(child, out);
    collectNamesExpr(stmt->while_cond, out);
    for (const auto& child : stmt->while_body) collectNamesStmt(child, out);
    collectNamesExpr(stmt->switch_expr, out);
    for (const auto& c : stmt->switch_cases) {
        if (c.value) collectNamesExpr(c.value.value(), out);
        for (const auto& child : c.body) collectNamesStmt(child, out);
    }
    for (const auto& child : stmt->block_stmts) collectNamesStmt(child, out);
    if (stmt->return_value) collectNamesExpr(stmt->return_value.value(), out);
    collectNamesExpr(stmt->expr_stmt, out);
}

void collectNamesExpr(const ExprPtr& expr, std::unordered_set<std::string>& out) {
    if (!expr) return;
    if (expr->kind == ExprKind::VarRef && !expr->var_name.empty()) out.insert(expr->var_name);
    collectNamesExpr(expr->left, out);
    collectNamesExpr(expr->right, out);
    collectNamesExpr(expr->operand, out);
    collectNamesExpr(expr->array_base, out);
    collectNamesExpr(expr->index, out);
    collectNamesExpr(expr->struct_base, out);
    collectNamesExpr(expr->cast_expr, out);
    collectNamesExpr(expr->cond, out);
    collectNamesExpr(expr->then_expr, out);
    collectNamesExpr(expr->else_expr, out);
    collectNamesExpr(expr->base, out);
    collectNamesExpr(expr->value, out);
    for (const auto& arg : expr->args) collectNamesExpr(arg, out);
    for (const auto& part : expr->parts) collectNamesExpr(part, out);
}

struct LowerResult {
    Operand operand;
    std::vector<S3StmtPtr> prelude;
};

class Lowerer {
public:
    explicit Lowerer(LowerContext ctx) : ctx_(std::move(ctx)) {}

    StatementizedFunction lowerFunction(const FunctionAST& fn) {
        StatementizedFunction out;
        out.name = fn.name;
        out.return_type = fn.return_type;
        out.params = fn.params;
        out.body = lowerStmtList(fn.body);
        return out;
    }

private:
    LowerContext ctx_;

    std::string tempHint(const ExprPtr& expr, const std::string& fallback) {
        if (!expr) return fallback;
        if (expr->kind == ExprKind::VarRef) return expr->var_name;
        if (expr->kind == ExprKind::Call && !expr->callee.empty()) return expr->callee;
        return fallback;
    }

    LValue tempLValue(TypeInfo type, const std::string& hint, DebugLoc loc,
                      std::vector<S3StmtPtr>& out) {
        std::string name = makeTempName(ctx_, hint);
        TypeInfo temp_type = type.name.empty() && type.hw_kind.empty() &&
                             type.width <= 0 && type.struct_name.empty()
            ? unknownType()
            : type;
        out.push_back(makeDecl(name, temp_type, loc));
        return varLValue(name, temp_type, loc);
    }

    Operand materializeOp(OpExpr op, const std::string& hint, DebugLoc loc,
                          std::vector<S3StmtPtr>& out) {
        TypeInfo type = op.type;
        auto target = tempLValue(type, hint, loc, out);
        Operand result = varOperand(target.root, target.type, loc);
        out.push_back(makeOp(std::move(target), std::move(op), loc));
        return result;
    }

    std::vector<Operand> lowerArgs(const std::vector<ExprPtr>& args,
                                   std::vector<S3StmtPtr>& out) {
        std::vector<Operand> lowered;
        lowered.reserve(args.size());
        for (const auto& arg : args) {
            auto part = lowerExpr(arg);
            out.insert(out.end(), part.prelude.begin(), part.prelude.end());
            lowered.push_back(std::move(part.operand));
        }
        return lowered;
    }

    LowerResult lowerExpr(const ExprPtr& expr) {
        if (!expr) return {};
        ErrorContextGuard guard("s3statementize", expr->debug_loc, "lowering expression");
        LowerResult result;
        switch (expr->kind) {
        case ExprKind::Literal:
            result.operand = literalOperand(expr->literal_value, expr->type, expr->debug_loc);
            return result;
        case ExprKind::VarRef:
            result.operand = varOperand(expr->var_name, expr->type, expr->debug_loc);
            return result;
        case ExprKind::ArrayAccess:
        case ExprKind::FieldAccess: {
            auto lv = lowerLValue(expr);
            result.prelude = std::move(lv.prelude);
            result.operand = lvalueOperand(std::move(lv.lvalue));
            return result;
        }
        case ExprKind::Call: {
            auto args = lowerArgs(expr->args, result.prelude);
            auto target = tempLValue(expr->type, tempHint(expr, "call"), expr->debug_loc,
                                     result.prelude);
            result.operand = varOperand(target.root, target.type, expr->debug_loc);
            if (isConstructorCall(expr, ctx_)) {
                result.prelude.push_back(makeConstruct(std::move(target), expr->callee,
                                                       std::move(args), expr->type,
                                                       expr->debug_loc));
            } else {
                result.prelude.push_back(makeCall(std::move(target), expr->callee,
                                                  std::move(args), expr->type,
                                                  expr->debug_loc));
            }
            return result;
        }
        case ExprKind::Cast: {
            auto value = lowerExpr(expr->cast_expr);
            result.prelude = std::move(value.prelude);
            OpExpr op;
            op.kind = OpExpr::Kind::Cast;
            op.type = expr->type;
            op.cast_type = expr->cast_type;
            op.debug_loc = expr->debug_loc;
            op.operands.push_back(std::move(value.operand));
            result.operand = materializeOp(std::move(op), "cast", expr->debug_loc, result.prelude);
            return result;
        }
        case ExprKind::UnaryOp: {
            if (expr->op == "++" || expr->op == "--" || expr->op == "post++" ||
                expr->op == "post--") {
                return lowerIncrement(expr);
            }
            auto value = lowerExpr(expr->operand);
            result.prelude = std::move(value.prelude);
            OpExpr op;
            op.kind = OpExpr::Kind::Unary;
            op.type = expr->type;
            op.debug_loc = expr->debug_loc;
            op.unary_op = parseUnaryOp(expr->op, expr->debug_loc);
            op.operands.push_back(std::move(value.operand));
            result.operand = materializeOp(std::move(op), "unary", expr->debug_loc, result.prelude);
            return result;
        }
        case ExprKind::BinaryOp: {
            if (expr->op == ",") fail(expr->debug_loc, "Comma expression is not supported");
            if (expr->op == "=") return lowerAssignmentExpr(expr);
            if (isCompoundAssign(expr->op)) return lowerCompoundAssignExpr(expr);
            if (expr->op == "&&" || expr->op == "||") return lowerShortCircuit(expr);
            auto lhs = lowerExpr(expr->left);
            result.prelude = std::move(lhs.prelude);
            auto rhs = lowerExpr(expr->right);
            result.prelude.insert(result.prelude.end(), rhs.prelude.begin(), rhs.prelude.end());
            OpExpr op;
            op.kind = OpExpr::Kind::Binary;
            op.type = expr->type;
            op.debug_loc = expr->debug_loc;
            op.binary_op = parseBinaryOp(expr->op, expr->debug_loc);
            op.operands.push_back(std::move(lhs.operand));
            op.operands.push_back(std::move(rhs.operand));
            result.operand = materializeOp(std::move(op), "binary", expr->debug_loc, result.prelude);
            return result;
        }
        case ExprKind::Ternary:
            return lowerTernary(expr);
        case ExprKind::ZExt:
        case ExprKind::SExt:
        case ExprKind::Trunc:
        case ExprKind::Slice:
        case ExprKind::BitSelect:
        case ExprKind::WriteSlice:
        case ExprKind::WriteBit:
        case ExprKind::DynamicWriteSlice:
        case ExprKind::DynamicWriteBit:
        case ExprKind::Concat:
        case ExprKind::Repeat:
        case ExprKind::ReduceOr:
        case ExprKind::ReduceAnd:
        case ExprKind::ReduceXor:
            return lowerHardwareExpr(expr);
        }
        return result;
    }

    struct LValueResult {
        LValue lvalue;
        std::vector<S3StmtPtr> prelude;
    };

    LValueResult lowerLValue(const ExprPtr& expr) {
        if (!expr) fail(DebugLoc{}, "Expected lvalue expression");
        if (expr->kind == ExprKind::VarRef) {
            return {varLValue(expr->var_name, expr->type, expr->debug_loc), {}};
        }
        if (expr->kind == ExprKind::FieldAccess) {
            auto base = lowerLValue(expr->struct_base);
            base.lvalue.accesses.push_back(
                LValueAccess{LValueAccessKind::Field, expr->field_name, nullptr});
            base.lvalue.type = expr->type;
            return base;
        }
        if (expr->kind == ExprKind::ArrayAccess) {
            auto base = lowerLValue(expr->array_base);
            auto idx = lowerExpr(expr->index);
            base.prelude.insert(base.prelude.end(), idx.prelude.begin(), idx.prelude.end());
            auto index = std::make_shared<Operand>(std::move(idx.operand));
            LValueAccess step;
            step.kind = LValueAccessKind::Index;
            step.index = std::move(index);
            base.lvalue.accesses.push_back(std::move(step));
            base.lvalue.type = expr->type;
            return base;
        }
        if (expr->kind == ExprKind::Cast) return lowerLValue(expr->cast_expr);
        fail(expr->debug_loc, "Expression is not a supported lvalue");
    }

    bool isCompoundAssign(const std::string& op) const {
        return op == "+=" || op == "-=" || op == "*=" || op == "/=" ||
               op == "%=" || op == "<<=" || op == ">>=" || op == "&=" ||
               op == "|=" || op == "^=";
    }

    bool exprIsSimpleOperand(const ExprPtr& expr) const {
        if (!expr) return false;
        switch (expr->kind) {
        case ExprKind::Literal:
        case ExprKind::VarRef:
            return true;
        case ExprKind::ArrayAccess:
        case ExprKind::FieldAccess:
            return !lvalueNeedsPrelude(expr);
        default:
            return false;
        }
    }

    bool lvalueNeedsPrelude(const ExprPtr& expr) const {
        if (!expr) return true;
        switch (expr->kind) {
        case ExprKind::VarRef:
            return false;
        case ExprKind::FieldAccess:
            return lvalueNeedsPrelude(expr->struct_base);
        case ExprKind::ArrayAccess:
            return lvalueNeedsPrelude(expr->array_base) ||
                   !exprIsSimpleOperand(expr->index);
        case ExprKind::Cast:
            return lvalueNeedsPrelude(expr->cast_expr);
        default:
            return true;
        }
    }

    std::string compoundBinaryOp(std::string op) const {
        op.pop_back();
        return op;
    }

    LowerResult lowerAssignmentExpr(const ExprPtr& expr) {
        LowerResult result;
        auto rhs = lowerExpr(expr->right);
        result.prelude = std::move(rhs.prelude);
        auto lhs = lowerLValue(expr->left);
        result.prelude.insert(result.prelude.end(), lhs.prelude.begin(), lhs.prelude.end());
        result.prelude.push_back(makeAssign(lhs.lvalue, rhs.operand, expr->debug_loc));
        result.operand = lvalueOperand(std::move(lhs.lvalue));
        return result;
    }

    LowerResult lowerCompoundAssignExpr(const ExprPtr& expr) {
        LowerResult result;
        auto lhs = lowerLValue(expr->left);
        result.prelude = std::move(lhs.prelude);
        auto rhs = lowerExpr(expr->right);
        result.prelude.insert(result.prelude.end(), rhs.prelude.begin(), rhs.prelude.end());
        OpExpr op;
        op.kind = OpExpr::Kind::Binary;
        op.type = expr->type;
        op.debug_loc = expr->debug_loc;
        op.binary_op = parseBinaryOp(compoundBinaryOp(expr->op), expr->debug_loc);
        op.operands.push_back(lvalueOperand(lhs.lvalue));
        op.operands.push_back(std::move(rhs.operand));
        auto temp = materializeOp(std::move(op), "compound", expr->debug_loc, result.prelude);
        result.prelude.push_back(makeAssign(lhs.lvalue, temp, expr->debug_loc));
        result.operand = lvalueOperand(std::move(lhs.lvalue));
        return result;
    }

    LowerResult lowerIncrement(const ExprPtr& expr) {
        LowerResult result;
        const bool post = expr->op == "post++" || expr->op == "post--";
        const bool inc = expr->op == "++" || expr->op == "post++";
        auto lv = lowerLValue(expr->operand);
        result.prelude = std::move(lv.prelude);
        if (post) {
            auto old = tempLValue(expr->type, "post", expr->debug_loc, result.prelude);
            result.prelude.push_back(makeAssign(old, lvalueOperand(lv.lvalue), expr->debug_loc));
            result.operand = varOperand(old.root, old.type, expr->debug_loc);
        }
        OpExpr op;
        op.kind = OpExpr::Kind::Binary;
        op.type = expr->type;
        op.debug_loc = expr->debug_loc;
        op.binary_op = inc ? BinaryOp::Add : BinaryOp::Sub;
        op.operands.push_back(lvalueOperand(lv.lvalue));
        op.operands.push_back(literalOperand("1", expr->type, expr->debug_loc));
        auto next = materializeOp(std::move(op), "inc", expr->debug_loc, result.prelude);
        result.prelude.push_back(makeAssign(lv.lvalue, next, expr->debug_loc));
        if (!post) result.operand = lvalueOperand(std::move(lv.lvalue));
        return result;
    }

    LowerResult lowerShortCircuit(const ExprPtr& expr) {
        LowerResult result;
        auto lhs = lowerExpr(expr->left);
        result.prelude = std::move(lhs.prelude);
        auto target = tempLValue(expr->type, "shortcircuit", expr->debug_loc, result.prelude);
        result.prelude.push_back(makeAssign(target, lhs.operand, expr->debug_loc));
        auto rhs = lowerExpr(expr->right);
        std::vector<S3StmtPtr> body = std::move(rhs.prelude);
        body.push_back(makeAssign(target, rhs.operand, expr->debug_loc));
        auto if_stmt = std::make_shared<S3Stmt>();
        if_stmt->kind = S3StmtKind::If;
        if_stmt->debug_loc = expr->debug_loc;
        if (expr->op == "&&") {
            if_stmt->condition = lhs.operand;
            if_stmt->then_body = std::move(body);
        } else {
            auto cond_target = tempLValue(expr->type, "logical_not", expr->debug_loc,
                                          result.prelude);
            OpExpr not_op;
            not_op.kind = OpExpr::Kind::Unary;
            not_op.type = expr->type;
            not_op.debug_loc = expr->debug_loc;
            not_op.unary_op = UnaryOp::LogicalNot;
            not_op.operands.push_back(lhs.operand);
            result.prelude.push_back(makeOp(cond_target, std::move(not_op), expr->debug_loc));
            if_stmt->condition = varOperand(cond_target.root, cond_target.type, expr->debug_loc);
            if_stmt->then_body = std::move(body);
        }
        result.prelude.push_back(if_stmt);
        result.operand = varOperand(target.root, target.type, expr->debug_loc);
        return result;
    }

    LowerResult lowerTernary(const ExprPtr& expr) {
        LowerResult result;
        auto cond = lowerExpr(expr->cond);
        result.prelude = std::move(cond.prelude);
        bool has_side_effect = exprHasSideEffect(expr->then_expr) ||
                               exprHasSideEffect(expr->else_expr);
        if (has_side_effect) {
            auto target = tempLValue(expr->type, "ternary", expr->debug_loc, result.prelude);
            auto then_value = lowerExpr(expr->then_expr);
            auto else_value = lowerExpr(expr->else_expr);
            auto if_stmt = std::make_shared<S3Stmt>();
            if_stmt->kind = S3StmtKind::If;
            if_stmt->debug_loc = expr->debug_loc;
            if_stmt->condition = cond.operand;
            if_stmt->then_body = std::move(then_value.prelude);
            if_stmt->then_body.push_back(makeAssign(target, then_value.operand, expr->debug_loc));
            if_stmt->else_body = std::move(else_value.prelude);
            if_stmt->else_body.push_back(makeAssign(target, else_value.operand, expr->debug_loc));
            result.prelude.push_back(if_stmt);
            result.operand = varOperand(target.root, target.type, expr->debug_loc);
            return result;
        }
        auto then_value = lowerExpr(expr->then_expr);
        auto else_value = lowerExpr(expr->else_expr);
        result.prelude.insert(result.prelude.end(),
                              then_value.prelude.begin(), then_value.prelude.end());
        result.prelude.insert(result.prelude.end(),
                              else_value.prelude.begin(), else_value.prelude.end());
        OpExpr op;
        op.kind = OpExpr::Kind::Ternary;
        op.type = expr->type;
        op.debug_loc = expr->debug_loc;
        op.operands.push_back(std::move(cond.operand));
        op.operands.push_back(std::move(then_value.operand));
        op.operands.push_back(std::move(else_value.operand));
        result.operand = materializeOp(std::move(op), "ternary", expr->debug_loc, result.prelude);
        return result;
    }

    bool exprHasSideEffect(const ExprPtr& expr) const {
        if (!expr) return false;
        switch (expr->kind) {
        case ExprKind::Call:
            if (isHardwareConstructorName(expr->callee)) {
                return std::any_of(expr->args.begin(), expr->args.end(),
                                   [&](const ExprPtr& arg) { return exprHasSideEffect(arg); });
            }
            return true;
        case ExprKind::BinaryOp:
            if (expr->op == "=" || isCompoundAssign(expr->op) ||
                expr->op == "&&" || expr->op == "||") {
                return true;
            }
            return exprHasSideEffect(expr->left) || exprHasSideEffect(expr->right);
        case ExprKind::UnaryOp:
            if (expr->op == "++" || expr->op == "--" ||
                expr->op == "post++" || expr->op == "post--") {
                return true;
            }
            return exprHasSideEffect(expr->operand);
        case ExprKind::ArrayAccess:
            return exprHasSideEffect(expr->array_base) || exprHasSideEffect(expr->index);
        case ExprKind::FieldAccess:
            return exprHasSideEffect(expr->struct_base);
        case ExprKind::Cast:
            return exprHasSideEffect(expr->cast_expr);
        case ExprKind::Ternary:
            return exprHasSideEffect(expr->cond) ||
                   exprHasSideEffect(expr->then_expr) ||
                   exprHasSideEffect(expr->else_expr);
        case ExprKind::ZExt:
        case ExprKind::SExt:
        case ExprKind::Trunc:
        case ExprKind::Slice:
        case ExprKind::BitSelect:
        case ExprKind::WriteSlice:
        case ExprKind::WriteBit:
        case ExprKind::DynamicWriteSlice:
        case ExprKind::DynamicWriteBit:
        case ExprKind::Concat:
        case ExprKind::Repeat:
        case ExprKind::ReduceOr:
        case ExprKind::ReduceAnd:
        case ExprKind::ReduceXor:
            if (exprHasSideEffect(expr->base) || exprHasSideEffect(expr->value) ||
                exprHasSideEffect(expr->operand) || exprHasSideEffect(expr->index) ||
                exprHasSideEffect(expr->cast_expr)) {
                return true;
            }
            return std::any_of(expr->parts.begin(), expr->parts.end(),
                               [&](const ExprPtr& part) { return exprHasSideEffect(part); });
        case ExprKind::Literal:
        case ExprKind::VarRef:
            return false;
        }
        return false;
    }

    HardwareOp hardwareKind(const ExprPtr& expr) {
        switch (expr->kind) {
        case ExprKind::ZExt: return HardwareOp::ZExt;
        case ExprKind::SExt: return HardwareOp::SExt;
        case ExprKind::Trunc: return HardwareOp::Trunc;
        case ExprKind::Slice: return HardwareOp::Slice;
        case ExprKind::BitSelect: return HardwareOp::BitSelect;
        case ExprKind::WriteSlice: return HardwareOp::WriteSlice;
        case ExprKind::WriteBit: return HardwareOp::WriteBit;
        case ExprKind::DynamicWriteSlice: return HardwareOp::DynamicWriteSlice;
        case ExprKind::DynamicWriteBit: return HardwareOp::DynamicWriteBit;
        case ExprKind::Concat: return HardwareOp::Concat;
        case ExprKind::Repeat: return HardwareOp::Repeat;
        case ExprKind::ReduceOr: return HardwareOp::ReduceOr;
        case ExprKind::ReduceAnd: return HardwareOp::ReduceAnd;
        case ExprKind::ReduceXor: return HardwareOp::ReduceXor;
        default: break;
        }
        fail(expr ? expr->debug_loc : DebugLoc{}, "Unsupported hardware expression");
    }

    LowerResult lowerHardwareExpr(const ExprPtr& expr) {
        LowerResult result;
        OpExpr op;
        op.kind = OpExpr::Kind::Hardware;
        op.type = expr->type;
        op.debug_loc = expr->debug_loc;
        op.hardware_op = hardwareKind(expr);
        op.hi = expr->hi;
        op.lo = expr->lo;
        op.bit = expr->bit;
        op.times = expr->times;
        op.to_width = expr->to_width;

        auto append = [&](const ExprPtr& child) {
            if (!child) return;
            auto lowered = lowerExpr(child);
            result.prelude.insert(result.prelude.end(), lowered.prelude.begin(), lowered.prelude.end());
            op.operands.push_back(std::move(lowered.operand));
        };
        append(expr->base);
        append(expr->value);
        append(expr->operand);
        append(expr->index);
        append(expr->cast_expr);
        for (const auto& part : expr->parts) append(part);
        result.operand = materializeOp(std::move(op), "hwop", expr->debug_loc, result.prelude);
        return result;
    }

    std::vector<S3StmtPtr> lowerStmtList(const std::vector<StmtPtr>& stmts) {
        std::vector<S3StmtPtr> out;
        for (const auto& stmt : stmts) {
            auto lowered = lowerStmt(stmt);
            out.insert(out.end(), lowered.begin(), lowered.end());
        }
        return out;
    }

    std::vector<S3StmtPtr> lowerStmt(const StmtPtr& stmt) {
        if (!stmt) return {};
        ErrorContextGuard guard("s3statementize", stmt->debug_loc, "lowering statement");
        std::vector<S3StmtPtr> out;
        switch (stmt->kind) {
        case StmtKind::Decl: {
            out.push_back(makeDecl(stmt->decl_name, stmt->decl_type, stmt->debug_loc));
            ctx_.used_names.insert(stmt->decl_name);
            if (stmt->decl_init) {
                if (stmt->decl_init.value()->kind == ExprKind::Call) {
                    auto init = stmt->decl_init.value();
                    auto args = lowerArgs(init->args, out);
                    auto target = varLValue(stmt->decl_name, stmt->decl_type, stmt->debug_loc);
                    if (isConstructorCall(init, ctx_)) {
                        out.push_back(makeConstruct(std::move(target), init->callee,
                                                    std::move(args), init->type,
                                                    init->debug_loc));
                    } else {
                        out.push_back(makeCall(std::move(target), init->callee,
                                               std::move(args), init->type,
                                               init->debug_loc));
                    }
                } else {
                    auto value = lowerExpr(stmt->decl_init.value());
                    out.insert(out.end(), value.prelude.begin(), value.prelude.end());
                    out.push_back(makeAssign(varLValue(stmt->decl_name, stmt->decl_type,
                                                       stmt->debug_loc),
                                             std::move(value.operand), stmt->debug_loc));
                }
            } else if (!stmt->decl_init_args.empty()) {
                auto args = lowerArgs(stmt->decl_init_args, out);
                out.push_back(makeConstruct(varLValue(stmt->decl_name, stmt->decl_type,
                                                      stmt->debug_loc),
                                            !stmt->decl_type.struct_name.empty()
                                                ? stmt->decl_type.struct_name
                                                : stmt->decl_type.name,
                                            std::move(args), stmt->decl_type,
                                            stmt->debug_loc));
            } else if (stmt->decl_default_constructed) {
                out.push_back(makeConstruct(varLValue(stmt->decl_name, stmt->decl_type,
                                                      stmt->debug_loc),
                                            !stmt->decl_type.struct_name.empty()
                                                ? stmt->decl_type.struct_name
                                                : stmt->decl_type.name,
                                            {}, stmt->decl_type, stmt->debug_loc));
            }
            return out;
        }
        case StmtKind::Assign: {
            if (stmt->assign_value && stmt->assign_value->kind == ExprKind::Call) {
                auto args = lowerArgs(stmt->assign_value->args, out);
                bool target_needs_prelude = lvalueNeedsPrelude(stmt->assign_target);
                if (target_needs_prelude) {
                    auto call_temp = tempLValue(stmt->assign_value->type,
                                                stmt->assign_value->callee,
                                                stmt->assign_value->debug_loc,
                                                out);
                    Operand call_value = varOperand(call_temp.root, call_temp.type,
                                                    stmt->assign_value->debug_loc);
                    if (isConstructorCall(stmt->assign_value, ctx_)) {
                        out.push_back(makeConstruct(std::move(call_temp),
                                                    stmt->assign_value->callee,
                                                    std::move(args),
                                                    stmt->assign_value->type,
                                                    stmt->debug_loc));
                    } else {
                        out.push_back(makeCall(std::move(call_temp),
                                               stmt->assign_value->callee,
                                               std::move(args),
                                               stmt->assign_value->type,
                                               stmt->debug_loc));
                    }
                    auto target = lowerLValue(stmt->assign_target);
                    out.insert(out.end(), target.prelude.begin(), target.prelude.end());
                    out.push_back(makeAssign(std::move(target.lvalue),
                                             std::move(call_value), stmt->debug_loc));
                } else {
                    auto target = lowerLValue(stmt->assign_target);
                    if (isConstructorCall(stmt->assign_value, ctx_)) {
                        out.push_back(makeConstruct(std::move(target.lvalue),
                                                    stmt->assign_value->callee,
                                                    std::move(args),
                                                    stmt->assign_value->type,
                                                    stmt->debug_loc));
                    } else {
                        out.push_back(makeCall(std::move(target.lvalue),
                                               stmt->assign_value->callee,
                                               std::move(args),
                                               stmt->assign_value->type,
                                               stmt->debug_loc));
                    }
                }
                return out;
            }
            auto rhs = lowerExpr(stmt->assign_value);
            out.insert(out.end(), rhs.prelude.begin(), rhs.prelude.end());
            auto lhs = lowerLValue(stmt->assign_target);
            out.insert(out.end(), lhs.prelude.begin(), lhs.prelude.end());
            out.push_back(makeAssign(std::move(lhs.lvalue), std::move(rhs.operand),
                                     stmt->debug_loc));
            return out;
        }
        case StmtKind::If: {
            auto cond = lowerExpr(stmt->if_cond);
            out.insert(out.end(), cond.prelude.begin(), cond.prelude.end());
            auto s = std::make_shared<S3Stmt>();
            s->kind = S3StmtKind::If;
            s->debug_loc = stmt->debug_loc;
            s->condition = std::move(cond.operand);
            s->then_body = lowerStmtList(stmt->if_then);
            s->else_body = lowerStmtList(stmt->if_else);
            out.push_back(s);
            return out;
        }
        case StmtKind::Block: {
            auto nested = lowerStmtList(stmt->block_stmts);
            out.insert(out.end(), nested.begin(), nested.end());
            return out;
        }
        case StmtKind::Return: {
            auto s = std::make_shared<S3Stmt>();
            s->kind = S3StmtKind::Return;
            s->debug_loc = stmt->debug_loc;
            if (stmt->return_value) {
                auto value = lowerExpr(stmt->return_value.value());
                out.insert(out.end(), value.prelude.begin(), value.prelude.end());
                s->return_value = std::move(value.operand);
            }
            out.push_back(s);
            return out;
        }
        case StmtKind::ExprStmt: {
            if (stmt->expr_stmt && stmt->expr_stmt->kind == ExprKind::Call) {
                auto args = lowerArgs(stmt->expr_stmt->args, out);
                if (isConstructorCall(stmt->expr_stmt, ctx_)) {
                    auto target = tempLValue(stmt->expr_stmt->type, "construct",
                                             stmt->debug_loc, out);
                    out.push_back(makeConstruct(std::move(target),
                                                stmt->expr_stmt->callee,
                                                std::move(args), stmt->expr_stmt->type,
                                                stmt->debug_loc));
                } else {
                    out.push_back(makeCall(std::nullopt, stmt->expr_stmt->callee,
                                           std::move(args), stmt->expr_stmt->type,
                                           stmt->debug_loc));
                }
                return out;
            }
            auto value = lowerExpr(stmt->expr_stmt);
            out.insert(out.end(), value.prelude.begin(), value.prelude.end());
            auto s = std::make_shared<S3Stmt>();
            s->kind = S3StmtKind::Eval;
            s->debug_loc = stmt->debug_loc;
            s->value = std::move(value.operand);
            out.push_back(s);
            return out;
        }
        case StmtKind::For: {
            auto s = std::make_shared<S3Stmt>();
            s->kind = S3StmtKind::For;
            s->debug_loc = stmt->debug_loc;
            if (stmt->for_init) s->for_init = lowerStmt(stmt->for_init);
            if (stmt->for_cond) {
                auto cond = lowerExpr(stmt->for_cond);
                s->condition_prelude = std::move(cond.prelude);
                s->for_cond = std::move(cond.operand);
            }
            if (stmt->for_step) {
                auto step = lowerExpr(stmt->for_step);
                s->for_step = std::move(step.prelude);
                auto eval = std::make_shared<S3Stmt>();
                eval->kind = S3StmtKind::Eval;
                eval->debug_loc = stmt->for_step->debug_loc;
                eval->value = std::move(step.operand);
                s->for_step.push_back(eval);
            }
            s->loop_body = lowerStmtList(stmt->for_body);
            out.push_back(s);
            return out;
        }
        case StmtKind::While:
        case StmtKind::DoWhile: {
            auto s = std::make_shared<S3Stmt>();
            s->kind = stmt->kind == StmtKind::While ? S3StmtKind::While : S3StmtKind::DoWhile;
            s->debug_loc = stmt->debug_loc;
            auto cond = lowerExpr(stmt->while_cond);
            s->condition_prelude = std::move(cond.prelude);
            s->condition = std::move(cond.operand);
            s->loop_body = lowerStmtList(stmt->while_body);
            out.push_back(s);
            return out;
        }
        case StmtKind::Switch: {
            auto selector = lowerExpr(stmt->switch_expr);
            out.insert(out.end(), selector.prelude.begin(), selector.prelude.end());
            auto s = std::make_shared<S3Stmt>();
            s->kind = S3StmtKind::Switch;
            s->debug_loc = stmt->debug_loc;
            s->switch_value = std::move(selector.operand);
            for (const auto& c : stmt->switch_cases) {
                S3CaseClause clause;
                if (c.value) {
                    auto value = lowerExpr(c.value.value());
                    out.insert(out.end(), value.prelude.begin(), value.prelude.end());
                    clause.value = std::move(value.operand);
                }
                clause.body = lowerStmtList(c.body);
                s->switch_cases.push_back(std::move(clause));
            }
            out.push_back(s);
            return out;
        }
        case StmtKind::Break: {
            auto s = std::make_shared<S3Stmt>();
            s->kind = S3StmtKind::Break;
            s->debug_loc = stmt->debug_loc;
            out.push_back(s);
            return out;
        }
        case StmtKind::Continue: {
            auto s = std::make_shared<S3Stmt>();
            s->kind = S3StmtKind::Continue;
            s->debug_loc = stmt->debug_loc;
            out.push_back(s);
            return out;
        }
        }
        return out;
    }
};

LowerContext makeContext(const FunctionAST& fn) {
    LowerContext ctx;
    ctx.function_name = fn.name;
    for (const auto& param : fn.params) ctx.used_names.insert(param.name);
    for (const auto& stmt : fn.body) collectNamesStmt(stmt, ctx.used_names);
    for (const auto& [name, _] : fn.struct_fields) {
        ctx.struct_names.insert(canonicalName(name));
    }
    for (const auto& [name, _] : fn.struct_constructors) {
        ctx.struct_names.insert(canonicalName(name));
    }
    return ctx;
}

StatementizedFunction lowerOneFunction(const FunctionAST& fn) {
    Lowerer lowerer(makeContext(fn));
    return lowerer.lowerFunction(fn);
}

std::string unaryName(UnaryOp op) {
    switch (op) {
    case UnaryOp::LogicalNot: return "LogicalNot";
    case UnaryOp::BitNot: return "BitNot";
    case UnaryOp::Negate: return "Negate";
    case UnaryOp::Plus: return "Plus";
    }
    return "Unary";
}

std::string binaryName(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "Add";
    case BinaryOp::Sub: return "Sub";
    case BinaryOp::Mul: return "Mul";
    case BinaryOp::Div: return "Div";
    case BinaryOp::Mod: return "Mod";
    case BinaryOp::Shl: return "Shl";
    case BinaryOp::Shr: return "Shr";
    case BinaryOp::BitAnd: return "BitAnd";
    case BinaryOp::BitOr: return "BitOr";
    case BinaryOp::BitXor: return "BitXor";
    case BinaryOp::LogicalAnd: return "LogicalAnd";
    case BinaryOp::LogicalOr: return "LogicalOr";
    case BinaryOp::Eq: return "Eq";
    case BinaryOp::Ne: return "Ne";
    case BinaryOp::Lt: return "Lt";
    case BinaryOp::Le: return "Le";
    case BinaryOp::Gt: return "Gt";
    case BinaryOp::Ge: return "Ge";
    }
    return "Binary";
}

std::string operandText(const Operand& op);

std::string lvalueText(const LValue& lv) {
    std::string out = lv.root;
    for (const auto& access : lv.accesses) {
        if (access.kind == LValueAccessKind::Field) {
            out += "." + access.field;
        } else {
            out += "[";
            out += access.index ? operandText(*access.index) : "<null>";
            out += "]";
        }
    }
    return out;
}

std::string operandText(const Operand& op) {
    switch (op.kind) {
    case OperandKind::Literal: return op.literal_value;
    case OperandKind::Var: return op.var_name;
    case OperandKind::LValueRead: return lvalueText(op.lvalue);
    }
    return "<operand>";
}

std::string opText(const OpExpr& op) {
    std::ostringstream os;
    if (op.kind == OpExpr::Kind::Unary) os << unaryName(op.unary_op);
    else if (op.kind == OpExpr::Kind::Binary) os << binaryName(op.binary_op);
    else if (op.kind == OpExpr::Kind::Ternary) os << "Ternary";
    else if (op.kind == OpExpr::Kind::Cast) os << "Cast";
    else os << "Hardware";
    os << "(";
    for (std::size_t i = 0; i < op.operands.size(); ++i) {
        if (i) os << ", ";
        os << operandText(op.operands[i]);
    }
    os << ")";
    return os.str();
}

void printStmtList(std::ostream& os, const std::vector<S3StmtPtr>& body, int indent);

std::string pad(int indent) {
    return std::string(static_cast<std::size_t>(indent), ' ');
}

void printStmt(std::ostream& os, const S3StmtPtr& stmt, int indent) {
    if (!stmt) return;
    os << pad(indent);
    switch (stmt->kind) {
    case S3StmtKind::Decl:
        os << "decl " << stmt->decl_name << "\n";
        return;
    case S3StmtKind::Assign:
        os << "assign " << lvalueText(stmt->target) << " = "
           << operandText(stmt->value) << "\n";
        return;
    case S3StmtKind::Op:
        os << "op " << lvalueText(stmt->target) << " = "
           << opText(stmt->op) << "\n";
        return;
    case S3StmtKind::Call:
        os << "call ";
        if (stmt->call_result) os << lvalueText(stmt->call_result.value()) << " = ";
        os << stmt->callee << "(";
        for (std::size_t i = 0; i < stmt->args.size(); ++i) {
            if (i) os << ", ";
            os << operandText(stmt->args[i]);
        }
        os << ")\n";
        return;
    case S3StmtKind::Construct:
        os << "construct " << lvalueText(stmt->target) << " = "
           << stmt->callee << "(";
        for (std::size_t i = 0; i < stmt->args.size(); ++i) {
            if (i) os << ", ";
            os << operandText(stmt->args[i]);
        }
        os << ")\n";
        return;
    case S3StmtKind::If:
        os << "if " << operandText(stmt->condition) << "\n";
        printStmtList(os, stmt->then_body, indent + 2);
        if (!stmt->else_body.empty()) {
            os << pad(indent) << "else\n";
            printStmtList(os, stmt->else_body, indent + 2);
        }
        return;
    case S3StmtKind::For:
        os << "for ";
        if (stmt->for_cond) os << operandText(stmt->for_cond.value());
        else os << "true";
        os << "\n";
        printStmtList(os, stmt->for_init, indent + 2);
        if (!stmt->condition_prelude.empty()) {
            os << pad(indent + 2) << "cond_prelude\n";
            printStmtList(os, stmt->condition_prelude, indent + 4);
        }
        printStmtList(os, stmt->loop_body, indent + 2);
        printStmtList(os, stmt->for_step, indent + 2);
        return;
    case S3StmtKind::While:
        os << "while " << operandText(stmt->condition) << "\n";
        if (!stmt->condition_prelude.empty()) {
            os << pad(indent + 2) << "cond_prelude\n";
            printStmtList(os, stmt->condition_prelude, indent + 4);
        }
        printStmtList(os, stmt->loop_body, indent + 2);
        return;
    case S3StmtKind::DoWhile:
        os << "do_while " << operandText(stmt->condition) << "\n";
        printStmtList(os, stmt->loop_body, indent + 2);
        if (!stmt->condition_prelude.empty()) {
            os << pad(indent + 2) << "cond_prelude\n";
            printStmtList(os, stmt->condition_prelude, indent + 4);
        }
        return;
    case S3StmtKind::Switch:
        os << "switch " << operandText(stmt->switch_value) << "\n";
        for (const auto& c : stmt->switch_cases) {
            os << pad(indent + 2)
               << (c.value ? "case " + operandText(c.value.value()) : "default") << "\n";
            printStmtList(os, c.body, indent + 4);
        }
        return;
    case S3StmtKind::Break:
        os << "break\n";
        return;
    case S3StmtKind::Continue:
        os << "continue\n";
        return;
    case S3StmtKind::Return:
        os << "return";
        if (stmt->return_value) os << " " << operandText(stmt->return_value.value());
        os << "\n";
        return;
    case S3StmtKind::Eval:
        os << "eval " << operandText(stmt->value) << "\n";
        return;
    }
}

void printStmtList(std::ostream& os, const std::vector<S3StmtPtr>& body, int indent) {
    for (const auto& stmt : body) printStmt(os, stmt, indent);
}

void verifyOperand(const Operand& op);

void verifyLValue(const LValue& lv) {
    if (lv.root.empty()) fail(lv.debug_loc, "Statementized lvalue has empty root");
    for (const auto& access : lv.accesses) {
        if (access.kind == LValueAccessKind::Index) {
            if (!access.index) fail(lv.debug_loc, "Statementized lvalue index is missing");
            verifyOperand(*access.index);
        }
    }
}

void verifyOperand(const Operand& op) {
    if (op.kind == OperandKind::Var && op.var_name.empty()) {
        fail(op.debug_loc, "Statementized operand var has empty name");
    }
    if (op.kind == OperandKind::LValueRead) verifyLValue(op.lvalue);
}

void verifyStmtList(const std::vector<S3StmtPtr>& body);

void verifyStmt(const S3StmtPtr& stmt) {
    if (!stmt) return;
    switch (stmt->kind) {
    case S3StmtKind::Decl:
    case S3StmtKind::Break:
    case S3StmtKind::Continue:
        return;
    case S3StmtKind::Assign:
        verifyLValue(stmt->target);
        verifyOperand(stmt->value);
        return;
    case S3StmtKind::Op:
        verifyLValue(stmt->target);
        for (const auto& operand : stmt->op.operands) verifyOperand(operand);
        return;
    case S3StmtKind::Call:
        if (stmt->call_result) verifyLValue(stmt->call_result.value());
        for (const auto& arg : stmt->args) verifyOperand(arg);
        return;
    case S3StmtKind::Construct:
        verifyLValue(stmt->target);
        for (const auto& arg : stmt->args) verifyOperand(arg);
        return;
    case S3StmtKind::If:
        verifyOperand(stmt->condition);
        verifyStmtList(stmt->then_body);
        verifyStmtList(stmt->else_body);
        return;
    case S3StmtKind::For:
        verifyStmtList(stmt->for_init);
        verifyStmtList(stmt->condition_prelude);
        if (stmt->for_cond) verifyOperand(stmt->for_cond.value());
        verifyStmtList(stmt->for_step);
        verifyStmtList(stmt->loop_body);
        return;
    case S3StmtKind::While:
    case S3StmtKind::DoWhile:
        verifyStmtList(stmt->condition_prelude);
        verifyOperand(stmt->condition);
        verifyStmtList(stmt->loop_body);
        return;
    case S3StmtKind::Switch:
        verifyOperand(stmt->switch_value);
        for (const auto& c : stmt->switch_cases) {
            if (c.value) verifyOperand(c.value.value());
            verifyStmtList(c.body);
        }
        return;
    case S3StmtKind::Return:
        if (stmt->return_value) verifyOperand(stmt->return_value.value());
        return;
    case S3StmtKind::Eval:
        verifyOperand(stmt->value);
        return;
    }
}

void verifyStmtList(const std::vector<S3StmtPtr>& body) {
    for (const auto& stmt : body) verifyStmt(stmt);
}

StatementizedProgram runStatementize(const FunctionAST& function) {
    StatementizedProgram program;
    program.struct_fields = function.struct_fields;
    program.struct_constructors = function.struct_constructors;
    program.top = lowerOneFunction(function);
    for (const auto& helper : function.helpers) {
        if (helper) program.helpers.push_back(lowerOneFunction(*helper));
    }
    for (const auto& [name, lambda] : function.lambdas) {
        if (lambda) program.lambdas[name] = lowerOneFunction(*lambda);
    }
    verifyStmtList(program.top.body);
    for (const auto& helper : program.helpers) verifyStmtList(helper.body);
    for (const auto& [_, lambda] : program.lambdas) verifyStmtList(lambda.body);
    return program;
}

} // namespace

std::string debugPrint(const StatementizedProgram& program) {
    std::ostringstream os;
    auto print_fn = [&](const StatementizedFunction& fn, const std::string& kind) {
        os << kind << " " << fn.name << "\n";
        printStmtList(os, fn.body, 2);
    };
    os << "s3statementize\n";
    print_fn(program.top, "top");
    for (const auto& helper : program.helpers) print_fn(helper, "helper");
    for (const auto& [name, lambda] : program.lambdas) {
        (void)name;
        print_fn(lambda, "lambda");
    }
    return os.str();
}

StatementizeResult statementizeFunctionAST(const FunctionAST& function,
                                           const StatementizeOptions& options) {
    try {
        auto program = runStatementize(function);
        StatementizeResult result;
        if (options.debug_print) result.debug_text = debugPrint(program);
        result.program = std::move(program);
        return result;
    } catch (const RTLZZException& ex) {
        StatementizeResult result;
        StatementizeError error;
        error.message = ex.message();
        error.formatted = ex.what();
        if (auto context = ex.primaryContext()) error.context = *context;
        result.error = std::move(error);
        return result;
    }
}

StatementizedProgram statementizeFunctionASTOrThrow(const FunctionAST& function,
                                                    const StatementizeOptions& options) {
    auto program = runStatementize(function);
    (void)options;
    return program;
}

} // namespace pred::s3statementize
