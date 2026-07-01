#pragma once

#include "debug/DebugLoc.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pred {
struct PredicateProgram;
}

namespace pred::beir {

using NodeId = std::uint64_t;
constexpr NodeId kInvalidNodeId = UINT64_MAX;

enum class OperandKind {
    Symbol,
    Literal,
    Port,
    Aggregate,
    LookupTable,
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
    Unknown,
};

enum class DebugOrigin {
    Source,
    Generated,
};

struct DebugInfo {
    DebugOrigin origin = DebugOrigin::Generated;
    std::vector<DebugLoc> source_locs;
    std::string reason;
    std::vector<NodeId> derived_nodes;
    std::vector<std::string> derived_names;

    bool hasSourceLoc() const;
};

struct ValueType {
    int width = 0;
    std::vector<int> array_dims;

    bool isArray() const { return !array_dims.empty(); }
};

struct Operand {
    OperandKind kind = OperandKind::Symbol;
    NodeId node = kInvalidNodeId;
    std::string text;
    struct Constant {
        std::vector<std::uint64_t> limbs;
        int width = 0;
        bool signed_view = false;

        bool isZero() const;
        bool isOne() const;
        bool isAllOnes() const;
        bool isBoolTrue() const;
        bool isBoolFalse() const;
        bool fitsU64() const;
        std::uint64_t toU64() const;
    } constant;
    ValueType type;
    bool signed_view = false;
};

struct Operation {
    OperationKind kind = OperationKind::Assign;
    OpCode op = OpCode::None;
    std::vector<Operand> operands;
    ValueType type;
    int to_width = 0;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    std::vector<DebugLoc> source_locs;
    DebugInfo debug;
};

struct Signal {
    NodeId id = kInvalidNodeId;
    std::string name;
    ValueType type;
    std::string port_name;
    int port_element_index = -1;
    DebugInfo debug;
    std::optional<Operation> driver;
};

struct Port {
    std::string name;
    PortDirection direction = PortDirection::Unknown;
    ValueType type;
    std::vector<NodeId> element_nodes;
};

struct Aggregate {
    std::string name;
    ValueType type;
    std::vector<NodeId> element_nodes;
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

    Signal* findSignal(NodeId id);
    const Signal* findSignal(NodeId id) const;
    Signal& signal(NodeId id);
    const Signal& signal(NodeId id) const;
};

Program buildProgram(const PredicateProgram& source);
std::string emitText(const Program& program);
std::string emitText(const PredicateProgram& source);

} // namespace pred::beir
