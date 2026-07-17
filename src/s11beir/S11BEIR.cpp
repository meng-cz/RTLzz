#include "s11beir/S11BEIR.h"

#include "backend/beopt.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pred::s11beir {
namespace {

using namespace pred::s10predicate;

ErrorContext makeContext(DebugLoc loc = {}, std::string note = {}) {
    ErrorContext context;
    context.stage = "s11beir";
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    context.note = std::move(note);
    return context;
}

[[noreturn]] void fail(const std::string& message,
                       DebugLoc loc = {},
                       std::string note = {}) {
    throwRTLZZ(makeContext(std::move(loc), std::move(note)), message);
}

bool isOutputPort(const S10Port& port) {
    return port.direction == ParamDirection::Output ||
           port.passing == ParamPassingKind::MutableRef;
}

beir::ValueType convertType(const S10Type& type) {
    if (type.width <= 0) fail("S10 type with invalid width reached S11");
    beir::ValueType out;
    out.width = type.width;
    return out;
}

bool sameType(const S10Type& lhs, const S10Type& rhs) {
    return lhs.kind == rhs.kind && lhs.width == rhs.width;
}

const S10Symbol& symbolAt(const S10PredicateProgram& program, SymbolId id) {
    if (id < 0 || id >= static_cast<SymbolId>(program.base_symbols.size())) {
        fail("S10 symbol id out of range");
    }
    return program.base_symbols[static_cast<std::size_t>(id)];
}

const S10Value& valueAt(const S10PredicateProgram& program, S10ValueId id) {
    if (id < 0 || id >= static_cast<S10ValueId>(program.values.size())) {
        fail("S10 value id out of range");
    }
    return program.values[static_cast<std::size_t>(id)];
}

std::string symbolName(const S10PredicateProgram& program, SymbolId id) {
    const auto& symbol = symbolAt(program, id);
    if (!symbol.debug_name.empty()) return symbol.debug_name;
    return "sym" + std::to_string(id);
}

std::string sanitizeNamePart(const std::string& text) {
    std::string out;
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            out.push_back(static_cast<char>(std::tolower(c)));
        } else if (ch == '_') {
            out.push_back('_');
        } else if (out.empty() || out.back() != '_') {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "tmp" : out;
}

std::string opKindName(S10OpKind kind) {
    switch (kind) {
    case S10OpKind::AssignCast: return "assign_cast";
    case S10OpKind::Add: return "add";
    case S10OpKind::Sub: return "sub";
    case S10OpKind::Mul: return "mul";
    case S10OpKind::Neg: return "neg";
    case S10OpKind::BitNot: return "bit_not";
    case S10OpKind::LogicalNot: return "logical_not";
    case S10OpKind::BitAnd: return "bit_and";
    case S10OpKind::BitOr: return "bit_or";
    case S10OpKind::BitXor: return "bit_xor";
    case S10OpKind::BoolAnd: return "bool_and";
    case S10OpKind::BoolOr: return "bool_or";
    case S10OpKind::Shl: return "shl";
    case S10OpKind::LShr: return "lshr";
    case S10OpKind::AShr: return "ashr";
    case S10OpKind::Eq: return "eq";
    case S10OpKind::Ne: return "ne";
    case S10OpKind::Lt: return "lt";
    case S10OpKind::Le: return "le";
    case S10OpKind::Gt: return "gt";
    case S10OpKind::Ge: return "ge";
    case S10OpKind::Mux: return "mux";
    case S10OpKind::ZExt: return "zext";
    case S10OpKind::SExt: return "sext";
    case S10OpKind::Trunc: return "trunc";
    case S10OpKind::Slice: return "slice";
    case S10OpKind::BitSelect: return "bit_select";
    case S10OpKind::DynamicSlice: return "dynamic_slice";
    case S10OpKind::DynamicBitSelect: return "dynamic_bit_select";
    case S10OpKind::WriteSlice: return "write_slice";
    case S10OpKind::WriteBit: return "write_bit";
    case S10OpKind::DynamicWriteSlice: return "dynamic_write_slice";
    case S10OpKind::DynamicWriteBit: return "dynamic_write_bit";
    case S10OpKind::Concat: return "concat";
    case S10OpKind::Repeat: return "repeat";
    case S10OpKind::ReduceOr: return "reduce_or";
    case S10OpKind::ReduceAnd: return "reduce_and";
    case S10OpKind::ReduceXor: return "reduce_xor";
    }
    return "op";
}

bool literalIsTrue(const S10Operand& operand) {
    if (operand.kind != S10OperandKind::Literal || operand.type.width != 1) return false;
    return !operand.literal.words.empty() && (operand.literal.words[0] & 1U) != 0;
}

std::string operandDebugText(const S10PredicateProgram& program,
                             const S10Operand& operand) {
    std::ostringstream os;
    if (operand.signed_view) os << "s:";
    if (operand.kind == S10OperandKind::Value) {
        const auto& value = valueAt(program, operand.value);
        os << (value.debug_name.empty() ? "v" + std::to_string(value.id) : value.debug_name)
           << "#" << value.id;
    } else {
        os << "literal";
        if (!operand.literal.source_text.empty()) os << "(" << operand.literal.source_text << ")";
        os << "<" << operand.type.width << ">";
    }
    return os.str();
}

beir::DebugInfo generatedDebug(std::string reason,
                               std::vector<beir::Operand> operands = {},
                               DebugLoc loc = {}) {
    beir::DebugInfo debug;
    debug.origin = loc.valid() ? beir::DebugOrigin::Source : beir::DebugOrigin::Generated;
    debug.reason = std::move(reason);
    if (loc.valid()) debug.source_locs.push_back(loc);
    for (const auto& operand : operands) {
        if (operand.kind == beir::OperandKind::Symbol &&
            operand.node != beir::kInvalidNodeId) {
            debug.derived_nodes.push_back(operand.node);
        } else if (!operand.text.empty()) {
            debug.derived_names.push_back(operand.text);
        }
    }
    return debug;
}

beir::Operand::Constant makeConstant(const S10Literal& literal,
                                     int width,
                                     bool signed_view) {
    beir::Operand::Constant constant;
    constant.width = width;
    constant.signed_view = signed_view;
    constant.limbs = literal.words;
    std::size_t words = width <= 0 ? 0 : static_cast<std::size_t>((width + 63) / 64);
    constant.limbs.resize(words, 0);
    if (!constant.limbs.empty() && width % 64 != 0) {
        constant.limbs.back() &= ((std::uint64_t{1} << (width % 64)) - 1);
    }
    return constant;
}

struct Builder {
    const S10PredicateProgram& input;
    BEIROptions options;
    beir::Program output;
    BEIRSummary summary;
    std::vector<beir::NodeId> value_nodes;
    std::unordered_map<SymbolId, beir::NodeId> output_nodes_by_symbol;
    std::unordered_map<SymbolId, S10ValueId> input_initial_by_symbol;
    std::unordered_set<std::string> used_names;
    int generated_nodes = 0;

    explicit Builder(const S10PredicateProgram& program, BEIROptions opts)
        : input(program), options(std::move(opts)) {
        summary.function_name = input.name;
        output.function_name = input.name;
        value_nodes.assign(input.values.size(), beir::kInvalidNodeId);
    }

    std::string uniqueName(std::string candidate) {
        candidate = sanitizeNamePart(candidate);
        std::string base = candidate;
        int suffix = 0;
        while (used_names.count(candidate)) {
            candidate = base + "_" + std::to_string(++suffix);
        }
        used_names.insert(candidate);
        return candidate;
    }

    beir::Signal& addSignal(std::string name,
                            beir::ValueType type,
                            beir::DebugInfo debug = {}) {
        beir::Signal signal;
        signal.id = static_cast<beir::NodeId>(output.signals.size());
        signal.name = uniqueName(std::move(name));
        signal.type = std::move(type);
        if (debug.reason.empty()) debug.reason = "allocated by S11 BEIR builder";
        signal.debug = std::move(debug);
        output.signals.push_back(std::move(signal));
        return output.signals.back();
    }

    beir::Signal& addGeneratedSignal(const std::string& stem,
                                     beir::ValueType type,
                                     const std::vector<beir::Operand>& operands,
                                     DebugLoc loc = {}) {
        ++generated_nodes;
        return addSignal("__s11_" + stem + "_" + std::to_string(generated_nodes),
                         std::move(type),
                         generatedDebug("generated by S11 " + stem, operands, std::move(loc)));
    }

    beir::Signal& signal(beir::NodeId id) {
        return output.signal(id);
    }

    const S10Definition* definitionFor(S10ValueId value) const {
        for (const auto& def : input.definitions) {
            if (def.target == value) return &def;
        }
        return nullptr;
    }

    beir::Operand valueOperand(S10ValueId value_id, bool signed_view = false) {
        const auto& value = valueAt(input, value_id);
        beir::NodeId node = value_nodes[static_cast<std::size_t>(value_id)];
        if (node == beir::kInvalidNodeId) fail("S11 value has no BEIR node");
        const auto& sig = output.signal(node);
        beir::Operand operand;
        operand.kind = beir::OperandKind::Symbol;
        operand.node = node;
        operand.text = sig.name;
        operand.type = sig.type;
        operand.signed_view = signed_view;
        if (!sameType(value.type, input.values[static_cast<std::size_t>(value_id)].type)) {
            fail("S11 internal value type mismatch");
        }
        return operand;
    }

    beir::Operand operand(const S10Operand& source) {
        beir::Operand out;
        out.type = convertType(source.type);
        out.signed_view = source.signed_view;
        if (source.kind == S10OperandKind::Value) {
            return valueOperand(source.value, source.signed_view);
        }
        out.kind = beir::OperandKind::Literal;
        out.constant = makeConstant(source.literal, source.type.width, source.signed_view);
        out.text = source.literal.source_text.empty()
            ? "literal"
            : source.literal.source_text;
        return out;
    }

    beir::Operand literalOperand(std::uint64_t value, S10Type type) {
        S10Operand source;
        source.kind = S10OperandKind::Literal;
        source.type = type;
        source.literal.valid_width = type.width;
        source.literal.words.push_back(value);
        return operand(source);
    }

    beir::Operand portOperand(const std::string& name, beir::ValueType type) {
        beir::Operand out;
        out.kind = beir::OperandKind::Port;
        out.text = name;
        out.type = std::move(type);
        return out;
    }

    beir::ValueType portGroupType(const S10PortGroup& group) const {
        beir::ValueType out;
        out.width = group.scalar_type.width;
        out.array_dims = group.array_dims;
        return out;
    }

    const S10Port* leafPortFor(SymbolId symbol) const {
        for (const auto& port : input.ports) {
            if (port.symbol == symbol) return &port;
        }
        return nullptr;
    }

    int flattenedArraySize(const std::vector<int>& dims) const {
        if (dims.empty()) return 1;
        int size = 1;
        for (int dim : dims) {
            if (dim <= 0) fail("S10 port group has invalid array dimension");
            size *= dim;
        }
        return size;
    }

    beir::Operation baseOperation(beir::OperationKind kind,
                                  beir::ValueType type,
                                  DebugLoc loc,
                                  std::string reason) {
        beir::Operation op;
        op.kind = kind;
        op.type = std::move(type);
        if (loc.valid()) op.source_locs.push_back(loc);
        op.debug = generatedDebug(std::move(reason), {}, loc);
        return op;
    }

    void setDriver(beir::NodeId node, beir::Operation op) {
        auto& sig = signal(node);
        if (!beir::sameType(sig.type, op.type)) {
            fail("S11 generated BEIR driver type mismatch", {},
                 "signal=" + sig.name);
        }
        if (sig.driver) fail("S11 attempted to assign a BEIR signal twice", {},
                             "signal=" + sig.name);
        sig.debug = op.debug;
        sig.driver = std::move(op);
    }

    std::string valueName(const S10Value& value) const {
        if (value.base_symbol >= 0) {
            std::string base = symbolName(input, value.base_symbol);
            if (value.version >= 0) return base + "_v" + std::to_string(value.version);
            return base + "_value" + std::to_string(value.id);
        }
        if (!value.debug_name.empty()) return value.debug_name + "_v" + std::to_string(value.id);
        return "s10_value_" + std::to_string(value.id);
    }

    void discoverInputInitials() {
        for (const auto& port : input.ports) {
            if (port.direction == ParamDirection::Input && port.initial_value) {
                input_initial_by_symbol[port.symbol] = *port.initial_value;
            }
        }
    }

    std::optional<std::string> inputPortNameForValue(S10ValueId value) const {
        const auto& s10_value = valueAt(input, value);
        if (s10_value.base_symbol < 0) return std::nullopt;
        auto it = input_initial_by_symbol.find(s10_value.base_symbol);
        if (it == input_initial_by_symbol.end() || it->second != value) return std::nullopt;
        return symbolName(input, s10_value.base_symbol);
    }

    void allocateValueSignals() {
        discoverInputInitials();
        for (const auto& value : input.values) {
            std::string name = valueName(value);
            if (auto input_port_name = inputPortNameForValue(value.id)) {
                name = *input_port_name;
            }
            beir::DebugInfo debug;
            debug.origin = value.kind == S10ValueKind::Generated
                ? beir::DebugOrigin::Generated
                : beir::DebugOrigin::Source;
            debug.reason = "S10 value " + std::to_string(value.id);
            if (!value.debug_name.empty()) debug.derived_names.push_back(value.debug_name);
            auto& signal = addSignal(std::move(name), convertType(value.type), std::move(debug));
            value_nodes[static_cast<std::size_t>(value.id)] = signal.id;
        }
    }

    void createLegacyPortsAndInputDrivers() {
        for (const auto& port : input.ports) {
            const std::string name = symbolName(input, port.symbol);
            const auto& symbol = symbolAt(input, port.symbol);

            beir::Port out_port;
            out_port.name = name;
            out_port.direction = port.direction == ParamDirection::Input
                ? beir::PortDirection::Input
                : beir::PortDirection::Output;
            out_port.type = convertType(symbol.type);

            if (port.direction == ParamDirection::Input) {
                if (!port.initial_value) {
                    fail("S10 input port has no initial value", {},
                         "port=" + name);
                }
                beir::NodeId node = value_nodes[static_cast<std::size_t>(*port.initial_value)];
                out_port.element_nodes.push_back(node);
                auto& sig = signal(node);
                sig.port_name = name;
                sig.port_element_index = 0;
                output.inputs.push_back(name);

                beir::Operation op = baseOperation(beir::OperationKind::PortRead,
                                                   sig.type,
                                                   {},
                                                   "read from S10 input port '" + name + "'");
                op.operands.push_back(portOperand(name, out_port.type));
                op.debug.derived_names.push_back("s10_input");
                setDriver(node, std::move(op));
                ++summary.input_ports;
            } else {
                auto& sig = addSignal(name, out_port.type,
                                      generatedDebug("S11 output port '" + name + "'"));
                sig.port_name = name;
                sig.port_element_index = 0;
                out_port.element_nodes.push_back(sig.id);
                output.outputs.push_back(name);
                output_nodes_by_symbol[port.symbol] = sig.id;
                ++summary.output_ports;
            }

            output.ports.push_back(std::move(out_port));
        }
    }

    void createGroupedPortsAndInputDrivers() {
        for (const auto& group : input.port_groups) {
            if (group.source_name.empty()) fail("S10 port group has empty source name");
            if (group.elements.empty()) {
                fail("S10 port group has no elements", {},
                     "port=" + group.source_name);
            }
            int expected = flattenedArraySize(group.array_dims);
            if (expected != static_cast<int>(group.elements.size())) {
                fail("S10 port group element count does not match array dimensions",
                     {},
                     "port=" + group.source_name);
            }

            beir::ValueType port_type = portGroupType(group);
            beir::ValueType scalar_type = port_type;
            scalar_type.array_dims.clear();

            beir::Port out_port;
            out_port.name = group.source_name;
            out_port.direction = group.direction == ParamDirection::Input
                ? beir::PortDirection::Input
                : beir::PortDirection::Output;
            out_port.type = port_type;

            if (group.direction == ParamDirection::Input) {
                output.inputs.push_back(group.source_name);
                ++summary.input_ports;
                for (std::size_t i = 0; i < group.elements.size(); ++i) {
                    const auto& element = group.elements[i];
                    const S10Port* leaf_port = leafPortFor(element.symbol);
                    if (!leaf_port || !leaf_port->initial_value) {
                        fail("S10 input port group element has no initial value",
                             {},
                             "port=" + group.source_name);
                    }
                    beir::NodeId node = value_nodes[static_cast<std::size_t>(*leaf_port->initial_value)];
                    out_port.element_nodes.push_back(node);
                    auto& sig = signal(node);
                    sig.port_name = group.source_name;
                    sig.port_element_index = static_cast<int>(i);

                    beir::Operation op = baseOperation(
                        beir::OperationKind::PortRead,
                        sig.type,
                        {},
                        "read from S10 input port '" + group.source_name + "'");
                    op.operands.push_back(portOperand(group.source_name, port_type));
                    if (!group.array_dims.empty()) {
                        op.operands.push_back(literalOperand(static_cast<std::uint64_t>(i),
                                                             S10Type{s8opnorm::S8TypeKind::Int, 32}));
                    }
                    op.debug.derived_names.push_back("s10_input");
                    setDriver(node, std::move(op));
                }
            } else {
                output.outputs.push_back(group.source_name);
                ++summary.output_ports;
                for (std::size_t i = 0; i < group.elements.size(); ++i) {
                    const auto& element = group.elements[i];
                    const S10Port* leaf_port = leafPortFor(element.symbol);
                    if (!leaf_port) {
                        fail("S10 output port group element has no leaf port",
                             {},
                             "port=" + group.source_name);
                    }
                    std::string element_name = group.array_dims.empty()
                        ? group.source_name
                        : group.source_name + "_" + std::to_string(i);
                    auto& sig = addSignal(element_name, scalar_type,
                                          generatedDebug("S11 output port '" +
                                                         group.source_name + "' element"));
                    sig.port_name = group.source_name;
                    sig.port_element_index = static_cast<int>(i);
                    out_port.element_nodes.push_back(sig.id);
                    output_nodes_by_symbol[element.symbol] = sig.id;
                }
            }

            output.ports.push_back(std::move(out_port));
        }
    }

    void createPortsAndInputDrivers() {
        if (input.port_groups.empty()) {
            createLegacyPortsAndInputDrivers();
            return;
        }
        createGroupedPortsAndInputDrivers();
    }

    beir::OpCode binaryOp(S10OpKind kind) {
        switch (kind) {
        case S10OpKind::Add: return beir::OpCode::Add;
        case S10OpKind::Sub: return beir::OpCode::Sub;
        case S10OpKind::Mul: return beir::OpCode::Mul;
        case S10OpKind::BitAnd: return beir::OpCode::BitAnd;
        case S10OpKind::BitOr: return beir::OpCode::BitOr;
        case S10OpKind::BitXor: return beir::OpCode::BitXor;
        case S10OpKind::BoolAnd: return beir::OpCode::LogicAnd;
        case S10OpKind::BoolOr: return beir::OpCode::LogicOr;
        case S10OpKind::Shl: return beir::OpCode::Shl;
        case S10OpKind::LShr:
        case S10OpKind::AShr: return beir::OpCode::Shr;
        case S10OpKind::Eq: return beir::OpCode::Eq;
        case S10OpKind::Ne: return beir::OpCode::Ne;
        case S10OpKind::Lt: return beir::OpCode::Lt;
        case S10OpKind::Le: return beir::OpCode::Le;
        case S10OpKind::Gt: return beir::OpCode::Gt;
        case S10OpKind::Ge: return beir::OpCode::Ge;
        default:
            fail("S10 op is not a BEIR binary op", {},
                 "op=" + opKindName(kind));
        }
    }

    beir::OpCode unaryOp(S10OpKind kind) {
        switch (kind) {
        case S10OpKind::Neg: return beir::OpCode::Neg;
        case S10OpKind::BitNot: return beir::OpCode::BitNot;
        case S10OpKind::LogicalNot: return beir::OpCode::LogicNot;
        default:
            fail("S10 op is not a BEIR unary op", {},
                 "op=" + opKindName(kind));
        }
    }

    bool isBinaryKind(S10OpKind kind) const {
        switch (kind) {
        case S10OpKind::Add:
        case S10OpKind::Sub:
        case S10OpKind::Mul:
        case S10OpKind::BitAnd:
        case S10OpKind::BitOr:
        case S10OpKind::BitXor:
        case S10OpKind::BoolAnd:
        case S10OpKind::BoolOr:
        case S10OpKind::Shl:
        case S10OpKind::LShr:
        case S10OpKind::AShr:
        case S10OpKind::Eq:
        case S10OpKind::Ne:
        case S10OpKind::Lt:
        case S10OpKind::Le:
        case S10OpKind::Gt:
        case S10OpKind::Ge:
            return true;
        default:
            return false;
        }
    }

    bool isUnaryKind(S10OpKind kind) const {
        return kind == S10OpKind::Neg ||
               kind == S10OpKind::BitNot ||
               kind == S10OpKind::LogicalNot;
    }

    beir::Operation convertOperation(const S10Operation& source, const S10Value& target) {
        beir::Operation op;
        op.type = convertType(target.type);
        op.to_width = source.result_width;
        op.hi = source.hi;
        op.lo = source.lo;
        op.bit = source.bit;
        op.times = source.times;
        if (source.debug_loc.valid()) op.source_locs.push_back(source.debug_loc);

        op.operands.reserve(source.operands.size());
        for (const auto& src_operand : source.operands) {
            op.operands.push_back(operand(src_operand));
        }
        if (source.kind == S10OpKind::AShr && !op.operands.empty()) {
            op.operands[0].signed_view = true;
        }

        if (source.kind == S10OpKind::AssignCast) {
            op.kind = beir::OperationKind::Cast;
        } else if (isBinaryKind(source.kind)) {
            op.kind = beir::OperationKind::Binary;
            op.op = binaryOp(source.kind);
        } else if (isUnaryKind(source.kind)) {
            op.kind = beir::OperationKind::Unary;
            op.op = unaryOp(source.kind);
        } else {
            switch (source.kind) {
            case S10OpKind::Mux: op.kind = beir::OperationKind::Ite; break;
            case S10OpKind::ZExt: op.kind = beir::OperationKind::ZExt; break;
            case S10OpKind::SExt:
                op.kind = beir::OperationKind::SExt;
                if (!op.operands.empty()) op.operands[0].signed_view = true;
                break;
            case S10OpKind::Trunc: op.kind = beir::OperationKind::Trunc; break;
            case S10OpKind::Slice: op.kind = beir::OperationKind::Slice; break;
            case S10OpKind::BitSelect: op.kind = beir::OperationKind::BitSelect; break;
            case S10OpKind::DynamicSlice: op.kind = beir::OperationKind::DynamicSlice; break;
            case S10OpKind::DynamicBitSelect: op.kind = beir::OperationKind::DynamicBitSelect; break;
            case S10OpKind::WriteSlice: op.kind = beir::OperationKind::WriteSlice; break;
            case S10OpKind::WriteBit: op.kind = beir::OperationKind::WriteBit; break;
            case S10OpKind::DynamicWriteSlice: op.kind = beir::OperationKind::DynamicWriteSlice; break;
            case S10OpKind::DynamicWriteBit: op.kind = beir::OperationKind::DynamicWriteBit; break;
            case S10OpKind::Concat: op.kind = beir::OperationKind::Concat; break;
            case S10OpKind::Repeat: op.kind = beir::OperationKind::Repeat; break;
            case S10OpKind::ReduceOr: op.kind = beir::OperationKind::ReduceOr; break;
            case S10OpKind::ReduceAnd: op.kind = beir::OperationKind::ReduceAnd; break;
            case S10OpKind::ReduceXor: op.kind = beir::OperationKind::ReduceXor; break;
            default:
                fail("Unsupported S10 op kind for S11 BEIR", source.debug_loc,
                     "op=" + opKindName(source.kind));
            }
        }

        op.debug = generatedDebug("S11 op " + opKindName(source.kind),
                                  op.operands,
                                  source.debug_loc);
        return op;
    }

    void lowerLookup(const S10Definition& def, const S10Value& target) {
        if (def.lookup_elements.empty()) {
            fail("S10 lookup has no elements", def.debug_loc);
        }
        ++summary.lowered_lookups;

        beir::Operand index = operand(def.lookup_index);
        std::vector<beir::Operand> elems;
        elems.reserve(def.lookup_elements.size());
        for (const auto& elem : def.lookup_elements) elems.push_back(operand(elem));

        beir::NodeId target_node = value_nodes[static_cast<std::size_t>(target.id)];
        if (elems.size() == 1) {
            beir::Operation op = baseOperation(beir::OperationKind::Assign,
                                               convertType(target.type),
                                               def.debug_loc,
                                               "S11 single-element lookup");
            op.operands.push_back(std::move(elems[0]));
            op.debug.derived_names.push_back("lowered_lookup");
            setDriver(target_node, std::move(op));
            return;
        }

        beir::ValueType scalar_type = convertType(target.type);
        beir::ValueType array_type = scalar_type;
        array_type.array_dims.push_back(static_cast<int>(elems.size()));

        beir::Operation aggregate = baseOperation(beir::OperationKind::Aggregate,
                                                  array_type,
                                                  def.debug_loc,
                                                  "S11 lookup table aggregate");
        aggregate.operands = std::move(elems);
        aggregate.debug = generatedDebug("S11 lookup table aggregate",
                                         aggregate.operands,
                                         def.debug_loc);
        aggregate.debug.derived_names.push_back("lowered_lookup_table");
        auto& array_signal = addGeneratedSignal("lookup_table",
                                                array_type,
                                                aggregate.operands,
                                                def.debug_loc);
        setDriver(array_signal.id, std::move(aggregate));
        ++summary.generated_lookup_nodes;

        beir::Operand array_operand;
        array_operand.kind = beir::OperandKind::Symbol;
        array_operand.node = array_signal.id;
        array_operand.text = array_signal.name;
        array_operand.type = array_signal.type;

        beir::Operation access = baseOperation(beir::OperationKind::ArrayAccess,
                                               scalar_type,
                                               def.debug_loc,
                                               "S11 lowered lookup array access");
        access.operands.push_back(std::move(array_operand));
        access.operands.push_back(std::move(index));
        access.debug = generatedDebug("S11 lowered lookup array access",
                                      access.operands,
                                      def.debug_loc);
        access.debug.derived_names.push_back("lowered_lookup_array_access");
        setDriver(target_node, std::move(access));
    }

    void emitDefinition(const S10Definition& def) {
        const auto& target = valueAt(input, def.target);
        beir::NodeId target_node = value_nodes[static_cast<std::size_t>(def.target)];
        switch (def.kind) {
        case S10DefKind::Assign: {
            beir::Operation op = baseOperation(beir::OperationKind::Assign,
                                               convertType(target.type),
                                               def.debug_loc,
                                               "S11 assign");
            op.operands.push_back(operand(def.value));
            op.debug = generatedDebug("S11 assign; guard erased from BEIR driver",
                                      op.operands,
                                      def.debug_loc);
            op.debug.derived_names.push_back("guard=" + operandDebugText(input, def.guard));
            if (!def.debug_note.empty()) op.debug.derived_names.push_back(def.debug_note);
            setDriver(target_node, std::move(op));
            break;
        }
        case S10DefKind::Op: {
            beir::Operation op = convertOperation(def.op, target);
            op.debug.reason += "; guard erased from BEIR driver";
            op.debug.derived_names.push_back("guard=" + operandDebugText(input, def.guard));
            if (!def.debug_note.empty()) op.debug.derived_names.push_back(def.debug_note);
            setDriver(target_node, std::move(op));
            break;
        }
        case S10DefKind::Lookup:
            lowerLookup(def, target);
            break;
        }
        ++summary.definitions;
    }

    void collectDeps(S10ValueId value,
                     const std::vector<const S10Definition*>& defs,
                     std::unordered_set<S10ValueId>& seen) const {
        if (seen.count(value)) return;
        seen.insert(value);
        if (value < 0 || value >= static_cast<S10ValueId>(defs.size())) return;
        const S10Definition* def = defs[static_cast<std::size_t>(value)];
        if (!def) return;
        auto add_operand = [&](const S10Operand& operand) {
            if (operand.kind == S10OperandKind::Value) collectDeps(operand.value, defs, seen);
        };
        switch (def->kind) {
        case S10DefKind::Assign:
            add_operand(def->value);
            break;
        case S10DefKind::Op:
            for (const auto& operand : def->op.operands) add_operand(operand);
            break;
        case S10DefKind::Lookup:
            add_operand(def->lookup_index);
            for (const auto& elem : def->lookup_elements) add_operand(elem);
            break;
        }
    }

    void rejectOutputInitialReads() const {
        std::vector<const S10Definition*> defs(input.values.size(), nullptr);
        for (const auto& def : input.definitions) {
            if (def.target >= 0 && def.target < static_cast<S10ValueId>(defs.size())) {
                defs[static_cast<std::size_t>(def.target)] = &def;
            }
        }

        std::unordered_set<S10ValueId> output_initials;
        for (const auto& port : input.ports) {
            if (isOutputPort(port) && port.initial_value) {
                output_initials.insert(*port.initial_value);
            }
        }
        if (output_initials.empty()) return;

        for (const auto& port : input.ports) {
            if (!isOutputPort(port) || !port.final_value) continue;
            std::unordered_set<S10ValueId> deps;
            collectDeps(*port.final_value, defs, deps);
            for (S10ValueId initial : output_initials) {
                if (deps.count(initial)) {
                    fail("S11 BEIR does not support reading output/mutable-ref initial values",
                         {},
                         "port=" + symbolName(input, port.symbol));
                }
            }
        }
    }

    void bindOutputs() {
        for (const auto& port : input.ports) {
            if (!isOutputPort(port)) continue;
            const std::string name = symbolName(input, port.symbol);
            if (!port.final_value) fail("S10 output port has no final value", {},
                                        "port=" + name);
            if (!port.final_guard || !literalIsTrue(*port.final_guard)) {
                fail("S11 requires output final guard to be total true", {},
                     "port=" + name);
            }
            auto node_it = output_nodes_by_symbol.find(port.symbol);
            if (node_it == output_nodes_by_symbol.end()) {
                fail("S11 output port node was not allocated", {}, "port=" + name);
            }
            const auto& final_value = valueAt(input, *port.final_value);
            beir::Operation op = baseOperation(beir::OperationKind::Assign,
                                               convertType(final_value.type),
                                               {},
                                               "S11 output final binding");
            op.operands.push_back(valueOperand(*port.final_value));
            op.debug = generatedDebug("S11 output '" + name + "' final binding",
                                      op.operands);
            setDriver(node_it->second, std::move(op));
        }
    }

    beir::Program build() {
        verifyPredicateProgram(input);
        rejectOutputInitialReads();
        allocateValueSignals();
        createPortsAndInputDrivers();
        for (const auto& def : input.definitions) emitDefinition(def);
        bindOutputs();

        summary.s10_values = static_cast<int>(input.values.size());
        summary.beir_signals = static_cast<int>(output.signals.size());
        beir::Program program = std::move(output);
        if (options.optimize) program = beir::opt::optimizeProgram(std::move(program));
        return program;
    }
};

std::string debugPrint(const beir::Program& program,
                       const std::vector<BEIRSummary>& summaries) {
    std::ostringstream os;
    os << "s11beir summaries\n";
    for (const auto& summary : summaries) {
        os << "  function " << summary.function_name
           << " input_ports=" << summary.input_ports
           << " output_ports=" << summary.output_ports
           << " s10_values=" << summary.s10_values
           << " beir_signals=" << summary.beir_signals
           << " definitions=" << summary.definitions
           << " lowered_lookups=" << summary.lowered_lookups
           << " generated_lookup_nodes=" << summary.generated_lookup_nodes
           << "\n";
    }
    os << "\n" << beir::emitText(program);
    return os.str();
}

} // namespace

BEIRResult buildBEIR(const S10PredicateProgram& program,
                     const BEIROptions& options) {
    try {
        BEIRResult result;
        Builder builder(program, options);
        beir::Program out = builder.build();
        result.summaries.push_back(builder.summary);
        if (options.debug_print) result.debug_text = debugPrint(out, result.summaries);
        result.program = std::move(out);
        return result;
    } catch (const RTLZZException& ex) {
        BEIRResult result;
        BEIRError error;
        error.context = ex.primaryContext().value_or(makeContext());
        error.message = ex.message();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    } catch (const std::exception& ex) {
        BEIRResult result;
        BEIRError error;
        error.context = makeContext();
        error.message = ex.what();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    }
}

beir::Program buildBEIROrThrow(const S10PredicateProgram& program,
                               const BEIROptions& options) {
    auto result = buildBEIR(program, options);
    if (!result.ok()) throw RTLZZException(result.error->context, result.error->message);
    return std::move(result.program.value());
}

} // namespace pred::s11beir
