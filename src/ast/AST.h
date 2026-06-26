#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pred {

// --- Type representation ---

struct TypeInfo {
    std::string name;       // e.g. "uint8_t", "int", "bool"
    int width = 0;          // bit width, 0 if unknown
    bool is_signed = false;
    bool is_hw_int = false;
    std::string hw_kind;     // "Int", "UInt", "bool", "builtin"
    bool is_pointer = false;
    bool is_reference = false;
    bool is_const = false;
    bool is_mutable = true;
    bool is_array = false;
    int array_size = 0;     // static array length
    std::vector<int> array_dims; // outer-to-inner static array dimensions
    std::string struct_name; // non-empty if struct type
    bool is_static = false;
    std::vector<std::string> init_values; // static/constant aggregate initializer values
};

enum class ParamPassingKind {
    Value,
    ConstRef,
    MutableRef,
    Pointer,
};

enum class ParamDirection {
    Input,
    Output,
    InOut,
};

inline std::string paramDirectionName(ParamDirection direction) {
    switch (direction) {
    case ParamDirection::Input: return "Input";
    case ParamDirection::Output: return "Output";
    case ParamDirection::InOut: return "InOut";
    }
    return "Input";
}

struct StructFieldInfo {
    std::string name;
    TypeInfo type;
};

struct StructConstructorInfo {
    std::vector<std::string> param_names;
    std::unordered_map<std::string, std::string> field_to_param;
};

// --- Expressions ---

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
    Concat,
    Repeat,
    ReduceOr,
    ReduceAnd,
    ReduceXor,
};

enum class IntrinsicKind {
    None,
    RegProxySetNext,
    ReqHelperOutput,
    DynamicRangeAt,
    DynamicBitAt,
};

struct Expr {
    ExprKind kind;
    TypeInfo type;

    // Literal
    std::string literal_value;

    // VarRef
    std::string var_name;

    // BinaryOp / UnaryOp
    std::string op;
    ExprPtr left;
    ExprPtr right;
    ExprPtr operand; // for unary

    // ArrayAccess
    ExprPtr array_base;
    ExprPtr index;

    // FieldAccess
    ExprPtr struct_base;
    std::string field_name;

    // Call
    std::string callee;
    std::vector<ExprPtr> args;
    IntrinsicKind intrinsic = IntrinsicKind::None;

    // Cast
    TypeInfo cast_type;
    ExprPtr cast_expr;

    // Ternary
    ExprPtr cond;
    ExprPtr then_expr;
    ExprPtr else_expr;

    // Hardware bitvector IR operations
    ExprPtr base;
    ExprPtr value;
    std::vector<ExprPtr> parts;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    int to_width = 0;
};

// --- Statements ---

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
    std::optional<ExprPtr> value; // nullopt = default
    std::vector<StmtPtr> body;
};

struct Stmt {
    StmtKind kind;

    // Assign: target = value
    ExprPtr assign_target;
    ExprPtr assign_value;

    // Decl: type name = init
    TypeInfo decl_type;
    std::string decl_name;
    std::optional<ExprPtr> decl_init;
    std::vector<ExprPtr> decl_init_args; // constructor/aggregate init args, in source order
    bool decl_default_constructed = false; // true when Clang saw an implicit C++ default construction

    // If
    ExprPtr if_cond;
    std::vector<StmtPtr> if_then;
    std::vector<StmtPtr> if_else;

    // For: init; cond; step; body
    StmtPtr for_init;
    ExprPtr for_cond;
    ExprPtr for_step;
    std::vector<StmtPtr> for_body;

    // While / DoWhile
    ExprPtr while_cond;
    std::vector<StmtPtr> while_body;

    // Switch
    ExprPtr switch_expr;
    std::vector<CaseClause> switch_cases;

    // Block
    std::vector<StmtPtr> block_stmts;

    // Return
    std::optional<ExprPtr> return_value;

    // ExprStmt
    ExprPtr expr_stmt;
};

// --- Function ---

struct ParamDecl {
    TypeInfo type;
    std::string name;
    bool is_output = false; // compatibility mirror for direction != Input
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
    bool is_const = false;
    bool is_pointer = false;
    bool is_reference = false;
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

// --- Helpers to construct nodes ---

inline ExprPtr make_literal(const std::string& val, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Literal;
    e->literal_value = val;
    e->type = type;
    return e;
}

inline ExprPtr make_var(const std::string& name, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::VarRef;
    e->var_name = name;
    e->type = type;
    return e;
}

inline ExprPtr make_binary(const std::string& op, ExprPtr lhs, ExprPtr rhs, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::BinaryOp;
    e->op = op;
    e->left = std::move(lhs);
    e->right = std::move(rhs);
    e->type = type;
    return e;
}

inline ExprPtr make_unary(const std::string& op, ExprPtr operand, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::UnaryOp;
    e->op = op;
    e->operand = std::move(operand);
    e->type = type;
    return e;
}

inline ExprPtr make_ternary(ExprPtr cond, ExprPtr t, ExprPtr f, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Ternary;
    e->cond = std::move(cond);
    e->then_expr = std::move(t);
    e->else_expr = std::move(f);
    e->type = type;
    return e;
}

inline ExprPtr make_array_access(ExprPtr base, ExprPtr idx, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::ArrayAccess;
    e->array_base = std::move(base);
    e->index = std::move(idx);
    e->type = type;
    return e;
}

inline ExprPtr make_field_access(ExprPtr base, const std::string& field, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::FieldAccess;
    e->struct_base = std::move(base);
    e->field_name = field;
    e->type = type;
    return e;
}

inline TypeInfo make_hw_type(const std::string& kind, int width, bool is_signed = false) {
    TypeInfo t;
    t.name = kind == "bool" ? "bool" : kind + "<" + std::to_string(width) + ">";
    t.width = width;
    t.is_hw_int = true;
    t.is_signed = is_signed;
    t.hw_kind = kind;
    return t;
}

inline TypeInfo make_bool_type() {
    TypeInfo t;
    t.name = "bool";
    t.width = 1;
    t.is_signed = false;
    t.hw_kind = "bool";
    return t;
}

inline TypeInfo make_bits_type(int width, bool is_signed = false) {
    TypeInfo t;
    t.name = (is_signed ? "Int<" : "UInt<") + std::to_string(width) + ">";
    t.width = width;
    t.is_signed = is_signed;
    t.is_hw_int = true;
    t.hw_kind = is_signed ? "Int" : "UInt";
    return t;
}

inline TypeInfo make_unknown_type(const std::string& name = "") {
    TypeInfo t;
    t.name = name;
    return t;
}

inline ExprPtr make_zext(ExprPtr expr, int to_width) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::ZExt;
    e->cast_expr = std::move(expr);
    e->to_width = to_width;
    e->type = make_hw_type("UInt", to_width, false);
    return e;
}

inline ExprPtr make_sext(ExprPtr expr, int to_width) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::SExt;
    e->cast_expr = std::move(expr);
    e->to_width = to_width;
    e->type = make_hw_type("Int", to_width, true);
    return e;
}

inline ExprPtr make_trunc(ExprPtr expr, int to_width, bool is_signed = false) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Trunc;
    e->cast_expr = std::move(expr);
    e->to_width = to_width;
    e->type = make_hw_type(is_signed ? "Int" : "UInt", to_width, is_signed);
    return e;
}

inline ExprPtr make_slice(ExprPtr base, int hi, int lo, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Slice;
    e->base = std::move(base);
    e->hi = hi;
    e->lo = lo;
    if (type.width <= 0) type = make_hw_type("UInt", hi >= lo ? hi - lo + 1 : 0, false);
    e->type = type;
    e->type.width = hi >= lo ? hi - lo + 1 : type.width;
    return e;
}

inline ExprPtr make_bit_select(ExprPtr base, int bit) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::BitSelect;
    e->base = std::move(base);
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
    e->type = type.width > 0 ? type : (e->base ? e->base->type : TypeInfo{});
    return e;
}

inline ExprPtr make_write_bit(ExprPtr base, int bit, ExprPtr value, TypeInfo type = {}) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::WriteBit;
    e->base = std::move(base);
    e->bit = bit;
    e->value = std::move(value);
    e->type = type.width > 0 ? type : (e->base ? e->base->type : TypeInfo{});
    return e;
}

inline ExprPtr make_concat(std::vector<ExprPtr> parts) {
    auto e = std::make_shared<Expr>();
    e->kind = ExprKind::Concat;
    e->parts = std::move(parts);
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
    e->type = make_hw_type("bool", 1, false);
    return e;
}

} // namespace pred
