#pragma once

#include "debug/DebugLoc.h"
#include "v2/V2Types.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s1apinorm {

using TypeInfo = pred::v2::TypeInfo;
using ParamDecl = pred::v2::ParamDecl;
using ParamDirection = pred::v2::ParamDirection;
using ParamPassingKind = pred::v2::ParamPassingKind;
using StructFieldInfo = pred::v2::StructFieldInfo;
using StructConstructorInfo = pred::v2::StructConstructorInfo;

struct S1Expr;
using S1ExprPtr = std::shared_ptr<S1Expr>;

enum class S1ExprKind {
    Literal,
    VarRef,
    BinaryOp,
    UnaryOp,
    ArrayAccess,
    FieldAccess,
    Call,
    Cast,
    Ternary,
    HardwareOp,
};

enum class S1HardwareOp {
    ZExt,
    SExt,
    Trunc,
    Slice,
    BitSelect,
    DynamicSlice,
    DynamicBitSelect,
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

struct S1Expr {
    S1ExprKind kind = S1ExprKind::Literal;
    TypeInfo type;
    DebugLoc debug_loc;

    std::string literal_value;
    std::string var_name;

    std::string op;
    S1ExprPtr left;
    S1ExprPtr right;
    S1ExprPtr operand;

    S1ExprPtr array_base;
    S1ExprPtr index;

    S1ExprPtr struct_base;
    std::string field_name;

    // After S1, Call means only a real constructor/helper/lambda call. Int API
    // pseudo-calls are lowered to HardwareOp.
    std::string callee;
    std::vector<S1ExprPtr> args;

    TypeInfo cast_type;
    S1ExprPtr cast_expr;

    S1ExprPtr cond;
    S1ExprPtr then_expr;
    S1ExprPtr else_expr;

    S1HardwareOp hardware_op = S1HardwareOp::Concat;
    S1ExprPtr base;
    S1ExprPtr value;
    std::vector<S1ExprPtr> parts;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    int to_width = 0;
};

struct S1Stmt;
using S1StmtPtr = std::shared_ptr<S1Stmt>;

enum class S1StmtKind {
    Decl,
    Assign,
    Construct,
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

struct S1CaseClause {
    std::optional<S1ExprPtr> value;
    std::vector<S1StmtPtr> body;
};

struct S1Stmt {
    S1StmtKind kind = S1StmtKind::ExprStmt;
    DebugLoc debug_loc;

    S1ExprPtr assign_target;
    S1ExprPtr assign_value;

    TypeInfo decl_type;
    std::string decl_name;
    bool decl_default_constructed = false;

    S1ExprPtr construct_target;
    std::string construct_callee;
    std::vector<S1ExprPtr> construct_args;
    TypeInfo construct_type;

    S1ExprPtr if_cond;
    std::vector<S1StmtPtr> if_then;
    std::vector<S1StmtPtr> if_else;

    std::vector<S1StmtPtr> for_init;
    S1ExprPtr for_cond;
    S1ExprPtr for_step;
    std::vector<S1StmtPtr> for_body;

    S1ExprPtr while_cond;
    std::vector<S1StmtPtr> while_body;

    S1ExprPtr switch_expr;
    std::vector<S1CaseClause> switch_cases;

    std::vector<S1StmtPtr> block_stmts;

    std::optional<S1ExprPtr> return_value;
    S1ExprPtr expr_stmt;
};

struct S1FunctionAST {
    std::string name;
    TypeInfo return_type;
    std::vector<ParamDecl> params;
    std::vector<S1StmtPtr> body;

    std::vector<std::shared_ptr<S1FunctionAST>> helpers;
    std::unordered_map<std::string, std::shared_ptr<S1FunctionAST>> lambdas;

    std::unordered_map<std::string, std::vector<StructFieldInfo>> struct_fields;
    std::unordered_map<std::string, std::vector<StructConstructorInfo>> struct_constructors;
};

const char* hardwareOpName(S1HardwareOp op);

} // namespace pred::s1apinorm
