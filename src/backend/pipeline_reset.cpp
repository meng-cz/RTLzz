#include "backend/pipeline.hpp"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/ArrayRef.h>
#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace pred::pipeline {
namespace {
using namespace beir;
using llvm::APInt;
using Value = std::vector<APInt>;

std::size_t elements(const ValueType& type) {
    std::size_t count = 1;
    if (type.width <= 0) throw std::runtime_error("pipeline reset: invalid value width");
    for (int dim : type.array_dims) {
        if (dim <= 0 || count > SIZE_MAX / static_cast<unsigned>(dim))
            throw std::runtime_error("pipeline reset: invalid array shape");
        count *= static_cast<unsigned>(dim);
    }
    return count;
}

Value decode(const ValueType& type, const std::vector<std::uint64_t>& words) {
    auto count = elements(type);
    auto stride = (static_cast<std::size_t>(type.width) + 63) / 64;
    if (count > SIZE_MAX / stride || words.size() != count * stride)
        throw std::runtime_error("pipeline reset: limb count does not match port shape");
    Value value;
    for (std::size_t i = 0; i < count; ++i) {
        if (type.width % 64 && (words[(i+1)*stride-1] >> (type.width % 64)))
            throw std::runtime_error("pipeline reset: high padding bits must be zero");
        value.emplace_back(type.width, llvm::ArrayRef<std::uint64_t>(words.data()+i*stride, stride));
    }
    return value;
}

std::vector<std::uint64_t> encode(const Value& value) {
    std::vector<std::uint64_t> words;
    for (const auto &element : value)
        words.insert(words.end(), element.getRawData(), element.getRawData()+element.getNumWords());
    return words;
}

class Evaluator {
    const beir::Program& program;
    std::vector<std::optional<Value>> cache;
    std::vector<bool> visiting;
    std::unordered_map<std::string, Value> inputs;

    Value operand(const Operand& op) {
        if (op.kind == OperandKind::Symbol) return signal(op.node);
        if (op.kind == OperandKind::Port) {
            auto it = inputs.find(op.text);
            if (it == inputs.end()) throw std::runtime_error("pipeline reset: unresolved port " + op.text);
            return it->second;
        }
        auto width = op.type.width > 0 ? op.type.width : op.constant.width;
        if (width <= 0) throw std::runtime_error("pipeline reset: malformed literal");
        auto words = op.constant.limbs;
        words.resize((width+63)/64, 0);
        return {APInt(width, llvm::ArrayRef<std::uint64_t>(words))};
    }

    Value operation(const Operation& op) {
        const unsigned width = op.type.width;
        auto scalar = [&](std::size_t i) {
            auto v = operand(op.operands.at(i));
            if (v.size() != 1) throw std::runtime_error("pipeline reset: scalar operation on array");
            return v.front();
        };
        auto signedView = [&](std::size_t i) { return op.operands.at(i).signed_view || op.operands.at(i).constant.signed_view; };
        auto boolean = [&](bool v) -> Value { return {APInt(width, v)}; };
        auto resized = [&](Value value, bool sign = false) {
            for (auto &v : value) v = sign ? v.sextOrTrunc(width) : v.zextOrTrunc(width);
            return value;
        };
        auto index = [&](std::size_t i, unsigned bits) {
            auto v = scalar(i);
            if (bits < v.getBitWidth()) v = v.trunc(bits);
            return v.getLimitedValue();
        };
        auto indexBits = [](std::size_t count) {
            unsigned bits = 1;
            while ((count-1) >> bits && bits < 63) ++bits;
            return bits;
        };
        switch (op.kind) {
        case OperationKind::Assign: case OperationKind::Cast: case OperationKind::ZExt: case OperationKind::Trunc: {
            auto value = resized(operand(op.operands.at(0)));
            if (op.signed_truncation) {
                auto original = operand(op.operands.at(0));
                for (std::size_t i=0; i<value.size(); ++i) if (width < original[i].getBitWidth()) {
                    value[i].clearBit(width-1);
                    if (original[i].isNegative()) value[i].setBit(width-1);
                }
            }
            return value;
        }
        case OperationKind::SExt: return resized(operand(op.operands.at(0)), true);
        case OperationKind::PortRead: {
            auto value = operand(op.operands.at(0));
            if (op.operands.size() == 1) return resized(value);
            auto i = scalar(1).getLimitedValue();
            if (i >= value.size()) throw std::runtime_error("pipeline reset: port index out of bounds");
            return {value[i].zextOrTrunc(width)};
        }
        case OperationKind::Ite: return resized(operand(op.operands.at(scalar(0).isZero() ? 2 : 1)));
        case OperationKind::Case:
            for (std::size_t i=0; i+1<op.operands.size(); i+=2)
                if (!scalar(i).isZero()) return resized(operand(op.operands.at(i+1)));
            return resized(operand(op.operands.back()));
        case OperationKind::Aggregate: {
            Value value;
            for (const auto &arg : op.operands) {
                auto part = resized(operand(arg));
                value.insert(value.end(), part.begin(), part.end());
            }
            return value;
        }
        case OperationKind::Lookup: case OperationKind::ArrayAccess: {
            auto array = operand(op.operands.at(0));
            auto i = index(1, indexBits(array.size()));
            if (i >= array.size()) throw std::runtime_error("pipeline reset: lookup index out of bounds");
            return {array[i].zextOrTrunc(width)};
        }
        case OperationKind::Unary: {
            auto x = scalar(0).zextOrTrunc(width);
            if (op.op == OpCode::Neg) return {-x};
            if (op.op == OpCode::BitNot) return {~x};
            if (op.op == OpCode::LogicNot) return boolean(scalar(0).isZero());
            break;
        }
        case OperationKind::Binary: {
            auto a = scalar(0), b = scalar(1);
            if (op.op == OpCode::LogicAnd) return boolean(!a.isZero() && !b.isZero());
            if (op.op == OpCode::LogicOr) return boolean(!a.isZero() || !b.isZero());
            if (op.op == OpCode::Shl || op.op == OpCode::Shr) {
                auto shift = b.getLimitedValue(a.getBitWidth());
                if (shift >= a.getBitWidth()) return {APInt(width, 0)};
                auto value = op.op == OpCode::Shl ? a.shl(shift) : signedView(0) ? a.ashr(shift) : a.lshr(shift);
                return {value.zextOrTrunc(width)};
            }
            const bool comparison = op.op == OpCode::Eq || op.op == OpCode::Ne || op.op == OpCode::Lt || op.op == OpCode::Le || op.op == OpCode::Gt || op.op == OpCode::Ge;
            unsigned common = comparison ? std::max(a.getBitWidth(), b.getBitWidth()) : width;
            const bool bitwise = op.op == OpCode::BitAnd || op.op == OpCode::BitOr || op.op == OpCode::BitXor;
            a = signedView(0) && !bitwise ? a.sextOrTrunc(common) : a.zextOrTrunc(common);
            b = signedView(1) && !bitwise ? b.sextOrTrunc(common) : b.zextOrTrunc(common);
            bool sign = signedView(0) || signedView(1);
            switch (op.op) {
            case OpCode::Add: return {a+b}; case OpCode::Sub: return {a-b}; case OpCode::Mul: return {a*b};
            case OpCode::BitAnd: return {a&b}; case OpCode::BitOr: return {a|b}; case OpCode::BitXor: return {a^b};
            case OpCode::Div: case OpCode::Mod:
                if (b.isZero()) throw std::runtime_error("pipeline reset: division by zero");
                return {op.op == OpCode::Div ? (sign ? a.sdiv(b) : a.udiv(b)) : (sign ? a.srem(b) : a.urem(b))};
            case OpCode::Eq: return boolean(a == b); case OpCode::Ne: return boolean(a != b);
            case OpCode::Lt: return boolean(sign ? a.slt(b) : a.ult(b));
            case OpCode::Le: return boolean(sign ? a.sle(b) : a.ule(b));
            case OpCode::Gt: return boolean(sign ? a.sgt(b) : a.ugt(b));
            case OpCode::Ge: return boolean(sign ? a.sge(b) : a.uge(b));
            default: break;
            }
            break;
        }
        case OperationKind::AddCarry: return {scalar(0).zextOrTrunc(width)+scalar(1).zextOrTrunc(width)+scalar(2).zextOrTrunc(width)};
        case OperationKind::Slice: case OperationKind::BitSelect:
            return {scalar(0).lshr(op.kind == OperationKind::Slice ? op.lo : op.bit).zextOrTrunc(width)};
        case OperationKind::DynamicSlice: case OperationKind::DynamicBitSelect: {
            auto value = scalar(0);
            auto i = index(1, indexBits(value.getBitWidth()));
            if (i+width > value.getBitWidth()) throw std::runtime_error("pipeline reset: dynamic read out of bounds");
            return {value.lshr(i).zextOrTrunc(width)};
        }
        case OperationKind::WriteSlice: case OperationKind::WriteBit: case OperationKind::DynamicWriteSlice: case OperationKind::DynamicWriteBit: {
            bool dynamic = op.kind == OperationKind::DynamicWriteSlice || op.kind == OperationKind::DynamicWriteBit;
            auto base = scalar(0).zextOrTrunc(width);
            auto part = scalar(dynamic ? 2 : 1);
            auto offset = dynamic ? index(1, indexBits(width)) : static_cast<std::uint64_t>(op.kind == OperationKind::WriteBit ? op.bit : op.lo);
            auto length = op.kind == OperationKind::WriteBit || op.kind == OperationKind::DynamicWriteBit ? 1u : dynamic ? part.getBitWidth() : static_cast<unsigned>(op.hi-op.lo+1);
            part = part.zextOrTrunc(length);
            for (unsigned i=0; i<length && offset+i<width; ++i) { base.clearBit(offset+i); if (part[i]) base.setBit(offset+i); }
            return {base};
        }
        case OperationKind::Concat: {
            APInt value(width, 0);
            for (const auto &arg : op.operands) {
                auto part = operand(arg);
                if (part.size()!=1) throw std::runtime_error("pipeline reset: array concat operand");
                value = value.shl(std::min(width, part[0].getBitWidth())) | part[0].zextOrTrunc(width);
            }
            return {value};
        }
        case OperationKind::Repeat: {
            auto part = scalar(0);
            APInt value(width, 0);
            for (int i=0; i<op.times; ++i) value = value.shl(std::min(width, part.getBitWidth())) | part.zextOrTrunc(width);
            return {value};
        }
        case OperationKind::ReduceOr: return boolean(!scalar(0).isZero());
        case OperationKind::ReduceAnd: return boolean(scalar(0).isAllOnes());
        case OperationKind::ReduceXor: return boolean(scalar(0).popcount() % 2);
        case OperationKind::Call: break;
        }
        throw std::runtime_error("pipeline reset: unsupported operation " + std::to_string(static_cast<int>(op.kind)));
    }
public:
    Evaluator(const beir::Program& graph, const Options& options)
        : program(graph), cache(graph.signals.size()), visiting(graph.signals.size(), false) {
        std::size_t count = 0;
        for (const auto &port : graph.ports) if (port.direction == PortDirection::Input) {
            ++count;
            auto it = options.input_reset_values.find(port.name);
            if (it == options.input_reset_values.end()) throw std::runtime_error("pipeline reset: missing input reset value: " + port.name);
            auto value = decode(port.type, it->second);
            if (port.element_nodes.size() != value.size()) throw std::runtime_error("pipeline reset: malformed input port elements");
            inputs.emplace(port.name, value);
            for (std::size_t i=0; i<value.size(); ++i) cache.at(port.element_nodes[i]) = Value{value[i]};
        }
        if (count != options.input_reset_values.size()) throw std::runtime_error("pipeline reset: unknown input reset port");
    }
    Value signal(NodeId id) {
        if (cache.at(id)) return *cache[id];
        const auto &node = program.signal(id);
        if (visiting.at(id) || !node.driver) throw std::runtime_error("pipeline reset: unresolved signal " + node.name);
        visiting[id] = true;
        auto value = operation(*node.driver);
        if (value.size() != elements(node.type)) throw std::runtime_error("pipeline reset: invalid computed shape: " + node.name);
        for (auto &v : value) v = v.zextOrTrunc(node.type.width);
        visiting[id] = false;
        cache[id] = value;
        return value;
    }
};
} // namespace

void initializeResetValues(Program& program) {
    if (!program.options.reset_enabled) return;
    // Evaluate the materialized DAG without changing it or running BEOPT across
    // registers. Register D assignments are transparent only in this evaluator.
    Evaluator evaluator(program.rtl_program, program.options);
    for (auto &reg : program.registers) reg.reset_values = encode(evaluator.signal(reg.node));
    for (const auto &port : program.rtl_program.ports) if (port.direction == PortDirection::Output) {
        Value values;
        for (auto id : port.element_nodes) {
            auto value = evaluator.signal(id);
            values.insert(values.end(), value.begin(), value.end());
        }
        program.report.output_reset_values[port.name] = encode(values);
    }
}
} // namespace pred::pipeline
