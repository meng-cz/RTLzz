#pragma once

#include "debug/DebugLoc.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
    Aggregate,
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

struct ValueFacts {
    bool valid = false;
    int width = 0;
    bool constant = false;
    Operand::Constant value;
    std::vector<std::uint64_t> known_zero;
    std::vector<std::uint64_t> known_one;
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
    ValueFacts value;
    std::optional<Operation> driver;
};

struct Port {
    std::string name;
    PortDirection direction = PortDirection::Unknown;
    ValueType type;
    std::vector<NodeId> element_nodes;
};

struct Program {
    std::string function_name;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::vector<Port> ports;
    std::vector<Signal> signals;

    Signal* findSignal(NodeId id);
    const Signal* findSignal(NodeId id) const;
    Signal& signal(NodeId id);
    const Signal& signal(NodeId id) const;
};

bool sameType(const ValueType& lhs, const ValueType& rhs);
bool isCommutativeOp(OperationKind kind, OpCode op);
void hashCombine(std::uint64_t& seed, std::uint64_t value);

struct TypeSignature {
    int width = 0;
    std::vector<int> array_dims;

    bool operator==(const TypeSignature& other) const;
    bool operator<(const TypeSignature& other) const;
};

struct TypeSignatureHash {
    std::size_t operator()(const TypeSignature& sig) const;
};

struct ConstantSignature {
    int width = 0;
    bool signed_view = false;
    std::vector<std::uint64_t> limbs;

    bool operator==(const ConstantSignature& other) const;
    bool operator<(const ConstantSignature& other) const;
};

struct OperandSignature {
    OperandKind kind = OperandKind::Symbol;
    NodeId node = kInvalidNodeId;
    std::uint64_t text_id = 0;
    TypeSignature type;
    bool signed_view = false;
    ConstantSignature constant;

    bool operator==(const OperandSignature& other) const;
    bool operator<(const OperandSignature& other) const;
};

struct OperandSignatureHash {
    std::size_t operator()(const OperandSignature& sig) const;
};

struct OperationSignature {
    OperationKind kind = OperationKind::Assign;
    OpCode op = OpCode::None;
    TypeSignature type;
    int to_width = 0;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    std::vector<OperandSignature> operands;

    bool operator==(const OperationSignature& other) const;
};

struct OperationSignatureHash {
    std::size_t operator()(const OperationSignature& sig) const;
};

class MutableProgram {
public:
    explicit MutableProgram(Program program);

    Program& program();
    const Program& program() const;
    Program finish();

    bool isObservable(const Signal& signal) const;
    bool isCseCandidate(const Operation& op) const;
    OperationSignature operationSignature(const Operation& op);
    Operand resolveOperand(Operand operand, const std::unordered_map<NodeId, Operand>& aliases) const;
    bool replaceAliases(const std::unordered_map<NodeId, Operand>& aliases);
    void compact(const std::unordered_set<NodeId>& live);
    void ensureValueFacts();
    void markValueFactsDirty();

private:
    Program program_;
    std::unordered_map<std::string, std::uint64_t> text_ids_;
    std::unordered_set<NodeId> observable_ids_;
    std::uint64_t next_text_id_ = 1;
    bool value_facts_dirty_ = true;

    void rebuildObservableIds();
    std::uint64_t internText(const std::string& text);
    TypeSignature typeSignature(const ValueType& type) const;
    ConstantSignature constantSignature(const Operand::Constant& constant) const;
    OperandSignature operandSignature(const Operand& operand);
    void remapDebug(DebugInfo& debug, const std::vector<NodeId>& remap);
    void remapOperand(Operand& operand, const std::vector<NodeId>& remap);
    void analyzeValueFacts();
};

Program buildProgram(const PredicateProgram& source, bool optimize = true);
std::string emitText(const Program& program);
std::string emitText(const PredicateProgram& source);

} // namespace pred::beir
