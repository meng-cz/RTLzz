#pragma once

#include "v2/V2Types.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <initializer_list>
#include <utility>
#include <vector>

namespace pred::v2 {

struct Expr;
using ExprPtr = std::shared_ptr<Expr>;

enum class ExprKind {
    Literal,
    VarRef,
    BinaryOp,
    UnaryOp,
    ArrayAccess,
    FieldAccess,
    Call,
    Cast,
    Ternary,
    ZExt,
    SExt,
    Trunc,
    Slice,
    BitSelect,
    WriteSlice,
    WriteBit,
    DynamicWriteSlice,
    DynamicWriteBit,
    Concat,
    Repeat,
    ReduceOr,
    ReduceAnd,
    ReduceXor,
};

enum class IntrinsicKind {
    None,
    DynamicRangeAt,
    DynamicBitAt,
};

struct Expr {
    ExprKind kind = ExprKind::Literal;
    TypeInfo type;
    DebugLoc debug_loc;

    std::string literal_value;
    std::string var_name;

    std::string op;
    ExprPtr left;
    ExprPtr right;
    ExprPtr operand;

    ExprPtr array_base;
    ExprPtr index;

    ExprPtr struct_base;
    std::string field_name;

    std::string callee;
    std::vector<ExprPtr> args;
    IntrinsicKind intrinsic = IntrinsicKind::None;

    TypeInfo cast_type;
    ExprPtr cast_expr;

    ExprPtr cond;
    ExprPtr then_expr;
    ExprPtr else_expr;

    ExprPtr base;
    ExprPtr value;
    std::vector<ExprPtr> parts;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    int to_width = 0;
};

struct Stmt;
using StmtPtr = std::shared_ptr<Stmt>;

enum class StmtKind {
    Assign,
    Decl,
    If,
    For,
    While,
    DoWhile,
    Switch,
    Block,
    Break,
    Continue,
    Return,
    ExprStmt,
};

struct CaseClause {
    std::optional<ExprPtr> value;
    std::vector<StmtPtr> body;
};

struct Stmt {
    StmtKind kind = StmtKind::ExprStmt;
    DebugLoc debug_loc;

    ExprPtr assign_target;
    ExprPtr assign_value;

    TypeInfo decl_type;
    std::string decl_name;
    std::optional<ExprPtr> decl_init;
    std::vector<ExprPtr> decl_init_args;
    bool decl_default_constructed = false;

    ExprPtr if_cond;
    std::vector<StmtPtr> if_then;
    std::vector<StmtPtr> if_else;

    StmtPtr for_init;
    ExprPtr for_cond;
    ExprPtr for_step;
    std::vector<StmtPtr> for_body;

    ExprPtr while_cond;
    std::vector<StmtPtr> while_body;

    ExprPtr switch_expr;
    std::vector<CaseClause> switch_cases;

    std::vector<StmtPtr> block_stmts;

    std::optional<ExprPtr> return_value;
    ExprPtr expr_stmt;
};

struct FunctionAST {
    std::string name;
    TypeInfo return_type;
    std::vector<ParamDecl> params;
    std::vector<StmtPtr> body;
    std::vector<std::shared_ptr<FunctionAST>> helpers;
    std::unordered_map<std::string, std::shared_ptr<FunctionAST>> lambdas;
    std::unordered_map<std::string, std::vector<StructFieldInfo>> struct_fields;
    std::unordered_map<std::string, std::vector<StructConstructorInfo>> struct_constructors;
};

inline DebugLoc firstDebugLoc(std::initializer_list<ExprPtr> exprs) {
    for (const auto& expr : exprs) {
        if (expr && expr->debug_loc.valid()) return expr->debug_loc;
    }
    return {};
}

inline DebugLoc firstDebugLoc(const std::vector<ExprPtr>& exprs) {
    for (const auto& expr : exprs) {
        if (expr && expr->debug_loc.valid()) return expr->debug_loc;
    }
    return {};
}

inline ExprPtr make_literal(const std::string& val, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Literal;
    e->literal_value = val;
    e->type = canonicalize_bool_type(std::move(type));
    return e;
}

inline ExprPtr make_var(const std::string& name, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::VarRef;
    e->var_name = name;
    e->type = canonicalize_bool_type(std::move(type));
    return e;
}

inline ExprPtr make_binary(const std::string& op, ExprPtr lhs, ExprPtr rhs, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::BinaryOp;
    e->op = op;
    e->left = std::move(lhs);
    e->right = std::move(rhs);
    e->debug_loc = firstDebugLoc({e->left, e->right});
    e->type = canonicalize_bool_type(std::move(type));
    return e;
}

inline ExprPtr make_unary(const std::string& op, ExprPtr operand, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::UnaryOp;
    e->op = op;
    e->operand = std::move(operand);
    e->debug_loc = firstDebugLoc({e->operand});
    e->type = canonicalize_bool_type(std::move(type));
    return e;
}

inline ExprPtr make_ternary(ExprPtr cond, ExprPtr t, ExprPtr f, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Ternary;
    e->cond = std::move(cond);
    e->then_expr = std::move(t);
    e->else_expr = std::move(f);
    e->debug_loc = firstDebugLoc({e->cond, e->then_expr, e->else_expr});
    e->type = canonicalize_bool_type(std::move(type));
    return e;
}

inline ExprPtr make_array_access(ExprPtr base, ExprPtr idx, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::ArrayAccess;
    e->array_base = std::move(base);
    e->index = std::move(idx);
    e->debug_loc = firstDebugLoc({e->array_base, e->index});
    e->type = canonicalize_bool_type(std::move(type));
    return e;
}

inline ExprPtr make_field_access(ExprPtr base, const std::string& field, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::FieldAccess;
    e->struct_base = std::move(base);
    e->field_name = field;
    e->debug_loc = firstDebugLoc({e->struct_base});
    e->type = canonicalize_bool_type(std::move(type));
    return e;
}

inline ExprPtr make_zext(ExprPtr expr, int to_width) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::ZExt;
    e->cast_expr = std::move(expr);
    e->debug_loc = firstDebugLoc({e->cast_expr});
    e->to_width = to_width;
    e->type = make_hw_type("UInt", to_width, false);
    return e;
}

inline ExprPtr make_sext(ExprPtr expr, int to_width) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::SExt;
    e->cast_expr = std::move(expr);
    e->debug_loc = firstDebugLoc({e->cast_expr});
    e->to_width = to_width;
    e->type = make_hw_type("Int", to_width, true);
    return e;
}

inline ExprPtr make_trunc(ExprPtr expr, int to_width, bool is_signed = false) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Trunc;
    e->cast_expr = std::move(expr);
    e->debug_loc = firstDebugLoc({e->cast_expr});
    e->to_width = to_width;
    e->type = make_hw_type(is_signed ? "Int" : "UInt", to_width, is_signed);
    return e;
}

inline ExprPtr make_slice(ExprPtr base, int hi, int lo, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Slice;
    e->base = std::move(base);
    e->debug_loc = firstDebugLoc({e->base});
    e->hi = hi;
    e->lo = lo;
    if (type.width <= 0) type = make_hw_type("UInt", hi >= lo ? hi - lo + 1 : 0, false);
    int slice_width = hi >= lo ? hi - lo + 1 : type.width;
    e->type = canonicalize_bool_type(std::move(type));
    e->type.width = slice_width;
    return e;
}

inline ExprPtr make_bit_select(ExprPtr base, int bit) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::BitSelect;
    e->base = std::move(base);
    e->debug_loc = firstDebugLoc({e->base});
    e->bit = bit;
    e->type = make_hw_type("bool", 1, false);
    return e;
}

inline ExprPtr make_write_slice(ExprPtr base, int hi, int lo, ExprPtr value, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::WriteSlice;
    e->base = std::move(base);
    e->hi = hi;
    e->lo = lo;
    e->value = std::move(value);
    e->debug_loc = firstDebugLoc({e->base, e->value});
    TypeInfo out_type = type.width > 0 ? type : (e->base ? e->base->type : TypeInfo{});
    e->type = canonicalize_bool_type(std::move(out_type));
    return e;
}

inline ExprPtr make_write_bit(ExprPtr base, int bit, ExprPtr value, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::WriteBit;
    e->base = std::move(base);
    e->bit = bit;
    e->value = std::move(value);
    e->debug_loc = firstDebugLoc({e->base, e->value});
    TypeInfo out_type = type.width > 0 ? type : (e->base ? e->base->type : TypeInfo{});
    e->type = canonicalize_bool_type(std::move(out_type));
    return e;
}

inline ExprPtr make_dynamic_write_slice(ExprPtr base, ExprPtr index, ExprPtr value, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::DynamicWriteSlice;
    e->base = std::move(base);
    e->index = std::move(index);
    e->value = std::move(value);
    e->debug_loc = firstDebugLoc({e->base, e->index, e->value});
    TypeInfo out_type = type.width > 0 ? type : (e->base ? e->base->type : TypeInfo{});
    e->type = canonicalize_bool_type(std::move(out_type));
    return e;
}

inline ExprPtr make_dynamic_write_bit(ExprPtr base, ExprPtr index, ExprPtr value, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::DynamicWriteBit;
    e->base = std::move(base);
    e->index = std::move(index);
    e->value = std::move(value);
    e->debug_loc = firstDebugLoc({e->base, e->index, e->value});
    TypeInfo out_type = type.width > 0 ? type : (e->base ? e->base->type : TypeInfo{});
    e->type = canonicalize_bool_type(std::move(out_type));
    return e;
}

inline ExprPtr make_concat(std::vector<ExprPtr> parts) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Concat;
    e->parts = std::move(parts);
    e->debug_loc = firstDebugLoc(e->parts);
    int width = 0;
    bool is_signed = false;
    for (auto& p : e->parts) {
        if (p) {
            width += p->type.width;
            is_signed = is_signed || p->type.is_signed;
        }
    }
    e->type = make_hw_type(is_signed ? "Int" : "UInt", width, is_signed);
    return e;
}

inline ExprPtr make_repeat(ExprPtr expr, int times) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Repeat;
    e->operand = std::move(expr);
    e->debug_loc = firstDebugLoc({e->operand});
    e->times = times;
    int width = (e->operand ? e->operand->type.width : 0) * times;
    bool is_signed = e->operand ? e->operand->type.is_signed : false;
    e->type = make_hw_type(is_signed ? "Int" : "UInt", width, is_signed);
    return e;
}

inline ExprPtr make_reduce(ExprKind kind, ExprPtr expr) {
    auto e = std::make_shared<Expr>();
    e->kind = kind;
    e->operand = std::move(expr);
    e->debug_loc = firstDebugLoc({e->operand});
    e->type = make_hw_type("bool", 1, false);
    return e;
}

} // namespace pred::v2
