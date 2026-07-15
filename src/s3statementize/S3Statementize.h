#pragma once

#include "ast/AST.h"
#include "debug/RTLZZException.h"
#include "s1apinorm/S1NormedAST.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s3statementize {

// SymbolId is the canonical variable identity from S3 onward.
// It is unique within one function across parameters, source locals, and
// compiler-generated temporaries. Later stages must use SymbolId rather than
// source names or lexical scopes to identify variables.
using SymbolId = int;
using ScopeId = int;

enum class UnaryOp {
    LogicalNot,
    BitNot,
    Negate,
    Plus,
};

enum class BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Shl,
    Shr,
    BitAnd,
    BitOr,
    BitXor,
    LogicalAnd,
    LogicalOr,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
};

enum class HardwareOp {
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

enum class LValueAccessKind {
    Field,
    Index,
};

struct Operand;

enum class S3ScopeKind {
    Function,
    Block,
    IfThen,
    IfElse,
    Loop,
    LoopBody,
    SwitchCase,
};

struct S3ScopeInfo {
    ScopeId id = -1;
    std::optional<ScopeId> parent;
    S3ScopeKind kind = S3ScopeKind::Block;
    std::string label;
};

struct SymbolInfo {
    SymbolId id = -1;
    std::string name;
    TypeInfo type;
    // S3/source provenance only. This is not a post-S3 semantic visibility
    // relation and later stages must not rely on it for name resolution or
    // variable identity.
    ScopeId declaring_scope = -1;
    std::vector<ScopeId> source_valid_scope_ids;
    bool is_param = false;
    bool is_temp = false;
};

struct LValueAccess {
    LValueAccessKind kind = LValueAccessKind::Field;
    std::string field;
    std::shared_ptr<Operand> index;
};

struct LValue {
    std::string root;
    SymbolId root_symbol = -1;
    TypeInfo type;
    std::vector<LValueAccess> accesses;
    DebugLoc debug_loc;
};

enum class OperandKind {
    Literal,
    Var,
    LValueRead,
};

struct Operand {
    OperandKind kind = OperandKind::Var;
    TypeInfo type;
    DebugLoc debug_loc;
    std::string literal_value;
    std::string var_name;
    SymbolId var_symbol = -1;
    LValue lvalue;
};

struct OpExpr {
    enum class Kind {
        Unary,
        Binary,
        Ternary,
        Cast,
        Hardware,
    };

    Kind kind = Kind::Unary;
    TypeInfo type;
    DebugLoc debug_loc;
    UnaryOp unary_op = UnaryOp::Plus;
    BinaryOp binary_op = BinaryOp::Add;
    HardwareOp hardware_op = HardwareOp::Concat;
    TypeInfo cast_type;
    std::vector<Operand> operands;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    int to_width = 0;
};

struct S3Stmt;
using S3StmtPtr = std::shared_ptr<S3Stmt>;

enum class S3StmtKind {
    Decl,
    Assign,
    Op,
    Call,
    Construct,
    If,
    For,
    While,
    DoWhile,
    Switch,
    Break,
    Continue,
    Return,
    Eval,
};

struct S3CaseClause {
    std::optional<Operand> value;
    std::vector<S3StmtPtr> body;
};

struct S3Stmt {
    S3StmtKind kind = S3StmtKind::Eval;
    DebugLoc debug_loc;

    TypeInfo decl_type;
    std::string decl_name;
    SymbolId decl_symbol = -1;
    bool decl_default_constructed = false;

    LValue target;
    Operand value;
    OpExpr op;

    std::optional<LValue> call_result;
    std::string callee;
    std::vector<Operand> args;
    TypeInfo result_type;

    Operand condition;
    std::vector<S3StmtPtr> condition_prelude;
    std::vector<S3StmtPtr> then_body;
    std::vector<S3StmtPtr> else_body;

    std::vector<S3StmtPtr> for_init;
    std::optional<Operand> for_cond;
    std::vector<S3StmtPtr> for_step;
    std::vector<S3StmtPtr> loop_body;

    Operand switch_value;
    std::vector<S3CaseClause> switch_cases;

    std::optional<Operand> return_value;
};

struct StatementizedFunction {
    std::string name;
    TypeInfo return_type;
    std::vector<ParamDecl> params;
    std::vector<S3ScopeInfo> scopes;
    std::vector<SymbolInfo> symbols;
    std::vector<S3StmtPtr> body;
};

struct StatementizedProgram {
    StatementizedFunction top;
    std::vector<StatementizedFunction> helpers;
    std::unordered_map<std::string, StatementizedFunction> lambdas;
    std::unordered_map<std::string, std::vector<StructFieldInfo>> struct_fields;
    std::unordered_map<std::string, std::vector<StructConstructorInfo>> struct_constructors;
};

struct StatementizeWarning {
    ErrorContext context;
    std::string message;
};

struct StatementizeError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct StatementizeOptions {
    bool debug_print = false;
};

struct StatementizeResult {
    std::optional<StatementizedProgram> program;
    std::optional<StatementizeError> error;
    std::vector<StatementizeWarning> warnings;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

StatementizeResult statementizeFunctionAST(
    const FunctionAST& function,
    const StatementizeOptions& options = {});

StatementizeResult statementizeFunctionAST(
    const s1apinorm::S1FunctionAST& function,
    const StatementizeOptions& options = {});

StatementizedProgram statementizeFunctionASTOrThrow(
    const FunctionAST& function,
    const StatementizeOptions& options = {});

StatementizedProgram statementizeFunctionASTOrThrow(
    const s1apinorm::S1FunctionAST& function,
    const StatementizeOptions& options = {});

std::string debugPrint(const StatementizedProgram& program);

} // namespace pred::s3statementize
