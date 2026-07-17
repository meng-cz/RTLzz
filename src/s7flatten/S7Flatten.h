#pragma once

#include "debug/RTLZZException.h"
#include "s6inline/S6Inline.h"

#include <optional>
#include <string>
#include <vector>

namespace pred::s7flatten {

using BlockId = int;
using SymbolId = int;

enum class S7SymbolRole {
    Local,
    Port,
    Temp,
};

struct S7Symbol {
    SymbolId id = -1;
    TypeInfo type;
    std::string debug_name;
    S7SymbolRole role = S7SymbolRole::Local;
};

struct S7Port {
    SymbolId symbol = -1;
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
};

struct S7PortElement {
    SymbolId symbol = -1;
    std::vector<int> indices;
};

struct S7PortGroup {
    std::string source_name;
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
    TypeInfo source_type;
    TypeInfo scalar_type;
    std::vector<int> array_dims;
    std::vector<S7PortElement> elements;
};

enum class S7OperandKind {
    Literal,
    Var,
};

struct S7Operand {
    S7OperandKind kind = S7OperandKind::Literal;
    TypeInfo type;
    // Use-level signed interpretation carried from S3. Symbols remain
    // function-unique storage objects; this flag belongs to this operand use.
    bool signed_view = false;
    DebugLoc debug_loc;
    std::string literal_value;
    SymbolId symbol = -1;
};

enum class S7UnaryOp {
    LogicalNot,
    BitNot,
    Negate,
    Plus,
};

enum class S7BinaryOp {
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

enum class S7HardwareOp {
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

enum class S7OpKind {
    Unary,
    Binary,
    Ternary,
    Cast,
    Hardware,
};

struct S7Operation {
    S7OpKind kind = S7OpKind::Unary;
    DebugLoc debug_loc;
    S7UnaryOp unary_op = S7UnaryOp::Plus;
    S7BinaryOp binary_op = S7BinaryOp::Add;
    S7HardwareOp hardware_op = S7HardwareOp::Concat;
    TypeInfo cast_type;
    std::vector<S7Operand> operands;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    int to_width = 0;
};

enum class S7StmtKind {
    Assign,
    Op,
    Lookup,
    LookupWrite,
};

struct S7Stmt {
    S7StmtKind kind = S7StmtKind::Assign;
    DebugLoc debug_loc;

    SymbolId target = -1;
    S7Operand value;
    S7Operation op;

    S7Operand lookup_index;
    std::vector<S7Operand> lookup_elements;
    std::vector<SymbolId> lookup_write_targets;
    S7Operand lookup_value;
};

struct S7SwitchTarget {
    std::optional<S7Operand> value;
    BlockId target = -1;
};

enum class S7TermKind {
    Jump,
    Branch,
    Switch,
    Exit,
    Unreachable,
};

struct S7Terminator {
    S7TermKind kind = S7TermKind::Unreachable;
    S7Operand condition;
    BlockId jump_target = -1;
    BlockId true_target = -1;
    BlockId false_target = -1;
    S7Operand switch_value;
    std::vector<S7SwitchTarget> switch_targets;
    BlockId default_target = -1;
};

struct S7BasicBlock {
    BlockId id = -1;
    std::vector<S7Stmt> stmts;
    S7Terminator terminator;
};

struct FlattenedCFG {
    std::string name;
    std::vector<S7Symbol> symbols;
    std::vector<S7Port> ports;
    std::vector<S7PortGroup> port_groups;
    BlockId entry = -1;
    BlockId exit = -1;
    std::vector<S7BasicBlock> blocks;
};

struct S7FlattenedProgram {
    FlattenedCFG top;
};

struct FlattenWarning {
    ErrorContext context;
    std::string message;
};

struct FlattenError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct FlattenOptions {
    bool debug_print = false;
    int max_leaf_symbols = 4096;
};

struct FlattenSummary {
    std::string function_name;
    int aggregate_symbols = 0;
    int leaf_symbols = 0;
    int dynamic_reads = 0;
    int dynamic_writes = 0;
};

struct FlattenResult {
    std::optional<S7FlattenedProgram> program;
    std::optional<FlattenError> error;
    std::vector<FlattenWarning> warnings;
    std::vector<FlattenSummary> summaries;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

FlattenResult flattenProgram(
    const s6inline::InlinedCFGProgram& program,
    const FlattenOptions& options = {});

S7FlattenedProgram flattenProgramOrThrow(
    const s6inline::InlinedCFGProgram& program,
    const FlattenOptions& options = {});

std::string debugPrint(const S7FlattenedProgram& program,
                       const std::vector<FlattenSummary>& summaries);

} // namespace pred::s7flatten
