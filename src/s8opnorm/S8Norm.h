#pragma once

#include "debug/RTLZZException.h"
#include "s7flatten/S7Flatten.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pred::s8opnorm {

using BlockId = int;
using SymbolId = int;

enum class S8TypeKind {
    Int,
    Bool,
};

struct S8Type {
    S8TypeKind kind = S8TypeKind::Int;
    int width = 0;
};

enum class S8SymbolRole {
    Local,
    Port,
    Temp,
};

struct S8Symbol {
    SymbolId id = -1;
    S8Type type;
    std::string debug_name;
    S8SymbolRole role = S8SymbolRole::Local;
};

struct S8Port {
    SymbolId symbol = -1;
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
};

struct S8PortElement {
    SymbolId symbol = -1;
    std::vector<int> indices;
};

struct S8PortGroup {
    std::string source_name;
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
    TypeInfo source_type;
    TypeInfo scalar_source_type;
    S8Type scalar_type;
    std::vector<int> array_dims;
    std::vector<S8PortElement> elements;
};

struct S8Literal {
    std::vector<std::uint64_t> words;
    int valid_width = 0;
    bool is_signed = false;
    std::string source_text;
};

enum class S8OperandKind {
    Literal,
    Var,
};

struct S8Operand {
    S8OperandKind kind = S8OperandKind::Literal;
    S8Type type;
    bool signed_view = false;
    DebugLoc debug_loc;
    S8Literal literal;
    SymbolId symbol = -1;
};

enum class S8OpKind {
    AssignCast,
    Add,
    Sub,
    Mul,
    Neg,
    BitNot,
    LogicalNot,
    BitAnd,
    BitOr,
    BitXor,
    BoolAnd,
    BoolOr,
    Shl,
    LShr,
    AShr,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Mux,
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

struct S8Operation {
    S8OpKind kind = S8OpKind::AssignCast;
    DebugLoc debug_loc;
    std::vector<S8Operand> operands;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    int result_width = 0;
};

enum class S8StmtKind {
    Assign,
    Op,
    Lookup,
    LookupWrite,
};

struct S8Stmt {
    S8StmtKind kind = S8StmtKind::Assign;
    DebugLoc debug_loc;
    SymbolId target = -1;
    S8Operand value;
    S8Operation op;
    S8Operand lookup_index;
    std::vector<S8Operand> lookup_elements;
    std::vector<SymbolId> lookup_write_targets;
    S8Operand lookup_value;
};

struct S8SwitchTarget {
    std::optional<S8Operand> value;
    BlockId target = -1;
};

enum class S8TermKind {
    Jump,
    Branch,
    Switch,
    Exit,
    Unreachable,
};

struct S8Terminator {
    S8TermKind kind = S8TermKind::Unreachable;
    S8Operand condition;
    BlockId jump_target = -1;
    BlockId true_target = -1;
    BlockId false_target = -1;
    S8Operand switch_value;
    std::vector<S8SwitchTarget> switch_targets;
    BlockId default_target = -1;
};

struct S8BasicBlock {
    BlockId id = -1;
    std::vector<S8Stmt> stmts;
    S8Terminator terminator;
};

struct S8NormCFG {
    std::string name;
    std::vector<S8Symbol> symbols;
    std::vector<S8Port> ports;
    std::vector<S8PortGroup> port_groups;
    BlockId entry = -1;
    BlockId exit = -1;
    std::vector<S8BasicBlock> blocks;
};

struct S8NormProgram {
    S8NormCFG top;
};

struct NormWarning {
    ErrorContext context;
    std::string message;
};

struct NormError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct NormOptions {
    bool debug_print = false;
    int max_symbols = 100000;
};

struct NormSummary {
    std::string function_name;
    int inserted_casts = 0;
    int normalized_ops = 0;
    int parsed_literals = 0;
};

struct NormResult {
    std::optional<S8NormProgram> program;
    std::optional<NormError> error;
    std::vector<NormWarning> warnings;
    std::vector<NormSummary> summaries;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

NormResult normalizeOperations(
    const s7flatten::S7FlattenedProgram& program,
    const NormOptions& options = {});

S8NormProgram normalizeOperationsOrThrow(
    const s7flatten::S7FlattenedProgram& program,
    const NormOptions& options = {});

void verifyNormProgram(const S8NormProgram& program);

std::string debugPrint(const S8NormProgram& program,
                       const std::vector<NormSummary>& summaries);

} // namespace pred::s8opnorm
