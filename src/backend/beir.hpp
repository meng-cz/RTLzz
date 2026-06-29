#pragma once

#include "ast/AST.h"
#include "predicate/PredicateIR.h"

#include <optional>
#include <string>
#include <vector>

namespace pred::beir {

enum class OperandKind {
    Symbol,
    Literal,
    Port,
    Aggregate,
};

enum class OperationKind {
    Assign,
    PortRead,
    Binary,
    Unary,
    ArrayAccess,
    Call,
    Cast,
    Ite,
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
    DynamicBitSelect,
    DynamicSlice,
    DynamicWriteSlice,
    DynamicWriteBit,
    Lookup,
};

enum class OpCode {
    None,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    BitAnd,
    BitOr,
    BitXor,
    LogicAnd,
    LogicOr,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Shl,
    Shr,
    LogicNot,
    BitNot,
    Neg,
};

enum class PortDirection {
    Input,
    Output,
    InOut,
    Unknown,
};

struct Operand {
    OperandKind kind = OperandKind::Symbol;
    std::string text;
    TypeInfo type;
};

struct Operation {
    OperationKind kind = OperationKind::Assign;
    OpCode op = OpCode::None;
    std::vector<Operand> operands;
    TypeInfo type;
    int to_width = 0;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    std::vector<DebugLoc> source_locs;
};

struct Signal {
    std::string name;
    TypeInfo type;
    std::string port_name;
    int port_element_index = -1;
    std::optional<Operation> driver;
};

struct Port {
    std::string name;
    PortDirection direction = PortDirection::Unknown;
    TypeInfo type;
    std::vector<std::string> element_symbols;
};

struct Aggregate {
    std::string name;
    TypeInfo type;
    std::vector<std::string> element_symbols;
};

struct LookupTable {
    std::string name;
    std::vector<std::string> values;
};

struct Program {
    std::string function_name;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::vector<Port> ports;
    std::vector<Aggregate> aggregates;
    std::vector<LookupTable> lookup_tables;
    std::vector<Signal> signals;
};

Program buildProgram(const PredicateProgram& source);
std::string emitText(const Program& program);
std::string emitText(const PredicateProgram& source);

} // namespace pred::beir
