#include "backend/rtlgen.hpp"
#include "predicate/PredicateIR.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace pred::rtlgen {
namespace {

using pred::beir::NodeId;

static int widthOf(const beir::ValueType& type) {
    return type.width > 0 ? type.width : 1;
}

static int flattenedArraySize(const beir::ValueType& type) {
    if (!type.array_dims.empty()) {
        int size = 1;
        for (int dim : type.array_dims) size *= dim;
        return size;
    }
    return 0;
}

static std::vector<int> arrayDims(const beir::ValueType& type) {
    if (!type.array_dims.empty()) return type.array_dims;
    return {};
}

static std::string sanitizeIdentifier(const std::string& text) {
    std::string out;
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c) || ch == '_') out.push_back(ch);
        else out.push_back('_');
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out.front()))) {
        out.insert(out.begin(), '_');
    }
    static const std::unordered_set<std::string> keywords = {
        "module", "endmodule", "input", "output", "inout", "logic", "wire",
        "assign", "always", "if", "else", "case", "endcase", "function",
        "endfunction", "signed", "unsigned", "localparam", "bit", "byte",
        "shortint", "int", "longint", "reg", "integer", "time", "local",
        "final"
    };
    if (keywords.count(out)) out += "_";
    return out;
}

static std::string packedRange(const beir::ValueType& type) {
    int width = widthOf(type);
    if (width <= 1) return "";
    return "[" + std::to_string(width - 1) + ":0] ";
}

static std::string unpackedDims(const beir::ValueType& type) {
    std::ostringstream os;
    for (int dim : arrayDims(type)) {
        os << " [0:" << (dim - 1) << "]";
    }
    return os.str();
}

static std::string logicType(const beir::ValueType& type) {
    return "logic " + packedRange(type);
}

static std::string constExpr(const beir::Operand::Constant& constant) {
    int width = constant.width > 0 ? constant.width : 1;
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    bool started = false;
    for (std::size_t i = constant.limbs.size(); i > 0; --i) {
        std::uint64_t limb = constant.limbs[i - 1];
        if (!started) {
            if (limb == 0 && i > 1) continue;
            hex << limb;
            started = true;
        } else {
            hex << std::setw(16) << limb;
        }
    }
    std::string digits = started ? hex.str() : "0";
    return std::to_string(width) + "'h" + digits;
}

static beir::Operand::Constant parseSmallConst(const std::string& text, int width) {
    beir::Operand::Constant constant;
    constant.width = width > 0 ? width : 32;
    constant.limbs.assign(static_cast<std::size_t>((constant.width + 63) / 64), 0);
    if (text == "true" || text == "false") {
        if (text == "true" && !constant.limbs.empty()) constant.limbs[0] = 1;
        return constant;
    }
    std::string value = text;
    while (!value.empty()) {
        char ch = value.back();
        if (ch == 'u' || ch == 'U' || ch == 'l' || ch == 'L') value.pop_back();
        else break;
    }
    std::uint64_t raw = static_cast<std::uint64_t>(std::stoull(value, nullptr, 0));
    if (!constant.limbs.empty()) constant.limbs[0] = raw;
    return constant;
}

static std::string zeroFor(const beir::ValueType& type) {
    return std::to_string(widthOf(type)) + "'h0";
}

static bool endsWithNumber(const std::string& name, std::string& base) {
    std::size_t pos = name.rfind('_');
    if (pos == std::string::npos || pos + 1 >= name.size()) return false;
    for (std::size_t i = pos + 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
    }
    base = name.substr(0, pos);
    return true;
}

static std::string svBinaryOp(beir::OpCode op) {
    switch (op) {
    case beir::OpCode::Add: return "+";
    case beir::OpCode::Sub: return "-";
    case beir::OpCode::Mul: return "*";
    case beir::OpCode::Div: return "/";
    case beir::OpCode::Mod: return "%";
    case beir::OpCode::BitAnd: return "&";
    case beir::OpCode::BitOr: return "|";
    case beir::OpCode::BitXor: return "^";
    case beir::OpCode::LogicAnd: return "&&";
    case beir::OpCode::LogicOr: return "||";
    case beir::OpCode::Eq: return "==";
    case beir::OpCode::Ne: return "!=";
    case beir::OpCode::Lt: return "<";
    case beir::OpCode::Le: return "<=";
    case beir::OpCode::Gt: return ">";
    case beir::OpCode::Ge: return ">=";
    case beir::OpCode::Shl: return "<<";
    case beir::OpCode::Shr: return ">>";
    default:
        throw std::runtime_error("rtlgen unsupported binary opcode");
    }
}

static void emitLocList(std::ostream& os, const std::vector<DebugLoc>& locs) {
    bool any = false;
    for (const auto& loc : locs) {
        if (!loc.valid()) continue;
        if (any) os << ", ";
        os << loc.file << ":" << loc.line << ":" << loc.column;
        if (loc.end_line || loc.end_column) {
            os << "-" << loc.end_line << ":" << loc.end_column;
        }
        any = true;
    }
    if (!any) os << "<none>";
}

class Emitter {
public:
    explicit Emitter(const beir::Program& program) : program_(program) {
        for (const auto& signal : program_.signals) {
            name_to_node_[signal.name] = signal.id;
        }
    }

    std::string emit() {
        std::ostringstream os;
        os << "`timescale 1ns/1ps\n\n";
        emitModuleHeader(os);
        emitSignalDecls(os);
        emitPortElementConnections(os);
        emitSignalAssignments(os);
        os << "endmodule\n";
        return os.str();
    }

private:
    const beir::Program& program_;
    std::unordered_map<std::string, NodeId> name_to_node_;

    std::string sig(NodeId id) const {
        return sanitizeIdentifier(program_.signal(id).name);
    }

    bool isDirectPortSignal(const beir::Signal& signal) const {
        return !signal.port_name.empty() && signal.name == signal.port_name;
    }

    std::string debugComment(const beir::DebugInfo& debug) const {
        std::ostringstream os;
        os << " // loc: ";
        emitLocList(os, debug.source_locs);
        os << "; message: ";

        if (debug.origin == beir::DebugOrigin::Source) {
            os << (debug.reason.empty() ? "direct source construct" : debug.reason);
            return os.str();
        }

        os << (debug.reason.empty() ? "intermediate generated value" : debug.reason);
        if (!debug.derived_nodes.empty()) {
            os << "; from signals ";
            for (std::size_t i = 0; i < debug.derived_nodes.size(); ++i) {
                if (i) os << ", ";
                NodeId id = debug.derived_nodes[i];
                os << "#" << id;
                if (const auto* signal = program_.findSignal(id)) {
                    os << ":" << sanitizeIdentifier(signal->name);
                }
            }
        }
        if (!debug.derived_names.empty()) {
            os << "; from names ";
            for (std::size_t i = 0; i < debug.derived_names.size(); ++i) {
                if (i) os << ", ";
                os << debug.derived_names[i];
            }
        }
        return os.str();
    }

    beir::DebugInfo signalDebug(const beir::Signal& signal) const {
        if (signal.debug.hasSourceLoc() ||
            (!signal.debug.reason.empty() && signal.debug.reason != "signal allocated by BEIR builder")) {
            return signal.debug;
        }
        if (signal.driver) return signal.driver->debug;
        std::string base;
        if (endsWithNumber(signal.name, base)) {
            auto it = name_to_node_.find(base);
            if (it != name_to_node_.end()) {
                const auto& base_signal = program_.signal(it->second);
                beir::DebugInfo debug = signal.debug;
                debug.source_locs.insert(debug.source_locs.end(),
                                         base_signal.debug.source_locs.begin(),
                                         base_signal.debug.source_locs.end());
                if (base_signal.driver) {
                    debug.source_locs.insert(debug.source_locs.end(),
                                             base_signal.driver->debug.source_locs.begin(),
                                             base_signal.driver->debug.source_locs.end());
                }
                if (debug.hasSourceLoc()) return debug;
            }
        }
        beir::DebugInfo debug;
        debug.origin = beir::DebugOrigin::Generated;
        debug.reason = "signal allocated without source location";
        return debug;
    }

    beir::DebugInfo portDebug(const beir::Port& port) const {
        beir::DebugInfo debug;
        debug.origin = beir::DebugOrigin::Source;
        debug.reason = "module port from source parameter '" + port.name + "'";
        for (NodeId id : port.element_nodes) {
            const auto& signal = program_.signal(id);
            if (!signal.driver) continue;
            debug.source_locs.insert(debug.source_locs.end(),
                                     signal.driver->debug.source_locs.begin(),
                                     signal.driver->debug.source_locs.end());
        }
        if (!debug.hasSourceLoc()) {
            debug.origin = beir::DebugOrigin::Generated;
            debug.reason = "module port generated from BEIR port '" + port.name + "'";
        }
        return debug;
    }

    void emitModuleHeader(std::ostream& os) const {
        os << "module " << sanitizeIdentifier(program_.function_name) << "(\n";
        for (std::size_t i = 0; i < program_.ports.size(); ++i) {
            const auto& port = program_.ports[i];
            os << "    ";
            switch (port.direction) {
            case beir::PortDirection::Input: os << "input "; break;
            case beir::PortDirection::Output: os << "output "; break;
            case beir::PortDirection::Unknown: os << "input "; break;
            }
            os << logicType(port.type) << sanitizeIdentifier(port.name)
               << unpackedDims(port.type);
            if (i + 1 < program_.ports.size()) os << ",";
            os << debugComment(portDebug(port));
            os << "\n";
        }
        os << ");\n\n";
    }

    void emitSignalDecls(std::ostream& os) const {
        for (const auto& signal : program_.signals) {
            if (isDirectPortSignal(signal)) continue;
            os << "    " << logicType(signal.type) << sanitizeIdentifier(signal.name)
               << unpackedDims(signal.type) << ";"
               << debugComment(signalDebug(signal)) << "\n";
        }
        if (!program_.signals.empty()) os << "\n";
    }

    void emitPortElementConnections(std::ostream& os) const {
        for (const auto& port : program_.ports) {
            if (port.direction == beir::PortDirection::Output) {
                for (std::size_t i = 0; i < port.element_nodes.size(); ++i) {
                    const auto& signal = program_.signal(port.element_nodes[i]);
                    if (isDirectPortSignal(signal)) continue;
                    os << "    assign " << sanitizeIdentifier(port.name) << "[" << i << "] = "
                       << sig(signal.id) << ";" << debugComment(signalDebug(signal)) << "\n";
                }
                continue;
            }
            if (port.type.isArray()) {
                for (std::size_t i = 0; i < port.element_nodes.size(); ++i) {
                    const auto& signal = program_.signal(port.element_nodes[i]);
                    os << "    assign " << sig(port.element_nodes[i]) << " = "
                       << sanitizeIdentifier(port.name) << "[" << i << "];"
                       << debugComment(signalDebug(signal)) << "\n";
                }
            }
        }
        os << "\n";
    }

    void emitSignalAssignments(std::ostream& os) const {
        for (const auto& signal : program_.signals) {
            if (signal.driver) {
                if (signal.driver->kind == beir::OperationKind::PortRead &&
                    isDirectPortSignal(signal)) {
                    continue;
                }
                if (signal.driver->kind == beir::OperationKind::PortRead &&
                    signal.port_element_index >= 0) {
                    continue;
                }
                if (signal.driver->kind == beir::OperationKind::Aggregate) {
                    emitAggregateAssignment(os, signal, *signal.driver);
                    continue;
                }
                os << "    assign " << sig(signal.id) << " = "
                   << resizeExpr(expr(*signal.driver),
                                 widthOf(signal.driver->type),
                                 widthOf(signal.type),
                                 false) << ";"
                   << debugComment(signal.driver->debug) << "\n";
                continue;
            }

            std::string base;
            if (endsWithNumber(signal.name, base)) {
                auto it = name_to_node_.find(base);
                if (it != name_to_node_.end()) {
                    os << "    assign " << sig(signal.id) << " = " << sig(it->second) << ";"
                       << debugComment(signalDebug(program_.signal(it->second))) << "\n";
                    continue;
                }
            }
            if (!isDirectPortSignal(signal) && !signal.type.isArray()) {
                beir::DebugInfo debug;
                debug.origin = beir::DebugOrigin::Generated;
                debug.reason = "default zero for undriven intermediate signal '" + signal.name + "'";
                os << "    assign " << sig(signal.id) << " = " << zeroFor(signal.type) << ";"
                   << debugComment(debug) << "\n";
            }
        }
    }

    void emitAggregateAssignment(std::ostream& os, const beir::Signal& signal, const beir::Operation& op) const {
        int count = flattenedArraySize(signal.type);
        if (count != static_cast<int>(op.operands.size())) {
            throw std::runtime_error("rtlgen aggregate operand count does not match array size");
        }
        for (int i = 0; i < count; ++i) {
            os << "    assign " << sig(signal.id) << "[" << i << "] = "
               << resizeExpr(operand(op.operands[static_cast<std::size_t>(i)]),
                             widthOf(op.operands[static_cast<std::size_t>(i)].type),
                             widthOf(signal.type),
                             false)
               << ";" << debugComment(op.debug) << "\n";
        }
    }

    std::string operand(const beir::Operand& op) const {
        switch (op.kind) {
        case beir::OperandKind::Symbol:
            return sig(op.node);
        case beir::OperandKind::Literal:
            return constExpr(op.constant);
        case beir::OperandKind::Port:
            return sanitizeIdentifier(op.text);
        }
        throw std::runtime_error("rtlgen unsupported operand kind");
    }

    std::string resizeExpr(std::string value, int from_width, int to_width, bool sign_extend) const {
        if (to_width <= 0 || from_width <= 0 || to_width == from_width) return value;
        if (to_width < from_width) {
            return std::to_string(to_width) + "'(" + value + ")";
        }
        int pad = to_width - from_width;
        if (sign_extend && from_width > 0) {
            return "({{" + std::to_string(pad) + "{" + value + "[" +
                   std::to_string(from_width - 1) + "]}}, " + value + "})";
        }
        return "({" + std::to_string(pad) + "'h0, " + value + "})";
    }

    std::string binaryOperand(const beir::Operand& op, bool signed_context) const {
        std::string value = operand(op);
        bool operand_is_signed = op.signed_view ||
                                 op.constant.signed_view;
        if (signed_context && operand_is_signed) return "$signed(" + value + ")";
        return value;
    }

    std::string expr(const beir::Operation& op) const {
        const auto& ops = op.operands;
        auto need = [&](std::size_t count) {
            if (ops.size() < count) throw std::runtime_error("rtlgen malformed operation operands");
        };

        switch (op.kind) {
        case beir::OperationKind::Assign:
            need(1);
            return resizeExpr(operand(ops[0]), widthOf(ops[0].type), widthOf(op.type), false);
        case beir::OperationKind::PortRead:
            need(1);
            if (ops.size() == 1) return operand(ops[0]);
            return operand(ops[0]) + "[" + operand(ops[1]) + "]";
        case beir::OperationKind::Binary:
            need(2);
            {
                bool signed_context =
                    ops[0].signed_view || ops[1].signed_view ||
                    ops[0].constant.signed_view || ops[1].constant.signed_view;
                if (op.op == beir::OpCode::Shr) {
                    signed_context = ops[0].signed_view || ops[0].constant.signed_view;
                }
                std::string sv_op = svBinaryOp(op.op);
                if (op.op == beir::OpCode::Shr && signed_context) sv_op = ">>>";
                return "(" + binaryOperand(ops[0], signed_context) + " " + sv_op + " " +
                       binaryOperand(ops[1], signed_context) + ")";
            }
        case beir::OperationKind::Unary:
            need(1);
            if (op.op == beir::OpCode::LogicNot) return "(!" + operand(ops[0]) + ")";
            if (op.op == beir::OpCode::BitNot) return "(~" + operand(ops[0]) + ")";
            if (op.op == beir::OpCode::Neg) return "(-" + operand(ops[0]) + ")";
            throw std::runtime_error("rtlgen unsupported unary opcode");
        case beir::OperationKind::Cast:
        case beir::OperationKind::ZExt:
        case beir::OperationKind::Trunc:
            need(1);
            return resizeExpr(operand(ops[0]), widthOf(ops[0].type), widthOf(op.type), false);
        case beir::OperationKind::SExt:
            need(1);
            return resizeExpr(operand(ops[0]), widthOf(ops[0].type), widthOf(op.type), true);
        case beir::OperationKind::Ite:
            need(3);
            return "(" + operand(ops[0]) + " ? " +
                   resizeExpr(operand(ops[1]), widthOf(ops[1].type), widthOf(op.type), false) +
                   " : " +
                   resizeExpr(operand(ops[2]), widthOf(ops[2].type), widthOf(op.type), false) +
                   ")";
        case beir::OperationKind::Slice:
            need(1);
            return std::to_string(op.hi - op.lo + 1) + "'((" + operand(ops[0]) +
                   ") >> " + std::to_string(op.lo) + ")";
        case beir::OperationKind::BitSelect:
            need(1);
            return "1'((" + operand(ops[0]) + ") >> " + std::to_string(op.bit) + ")";
        case beir::OperationKind::WriteSlice:
            need(2);
            return writeSliceExpr(operand(ops[0]), operand(ops[1]), widthOf(op.type),
                                  op.lo, op.hi - op.lo + 1, widthOf(ops[1].type));
        case beir::OperationKind::WriteBit:
            need(2);
            return writeSliceExpr(operand(ops[0]), operand(ops[1]), widthOf(op.type),
                                  op.bit, 1, widthOf(ops[1].type));
        case beir::OperationKind::DynamicBitSelect:
            need(2);
            return "1'(" + operand(ops[0]) + " >> " + operand(ops[1]) + ")";
        case beir::OperationKind::DynamicSlice:
            need(2);
            return "(" + operand(ops[0]) + " >> " + operand(ops[1]) + ")";
        case beir::OperationKind::DynamicWriteSlice:
            need(3);
            return writeSliceExpr(operand(ops[0]), operand(ops[2]), widthOf(op.type),
                                  operand(ops[1]), widthOf(ops[2].type), widthOf(ops[2].type));
        case beir::OperationKind::DynamicWriteBit:
            need(3);
            return writeSliceExpr(operand(ops[0]), operand(ops[2]), widthOf(op.type),
                                  operand(ops[1]), 1, widthOf(ops[2].type));
        case beir::OperationKind::Concat:
            return concatExpr(ops);
        case beir::OperationKind::Repeat:
            need(1);
            return "{" + std::to_string(op.times) + "{" + operand(ops[0]) + "}}";
        case beir::OperationKind::ReduceOr:
            need(1);
            return "(|" + operand(ops[0]) + ")";
        case beir::OperationKind::ReduceAnd:
            need(1);
            return "(&" + operand(ops[0]) + ")";
        case beir::OperationKind::ReduceXor:
            need(1);
            return "(^" + operand(ops[0]) + ")";
        case beir::OperationKind::Lookup:
        case beir::OperationKind::ArrayAccess:
            need(2);
            return operand(ops[0]) + "[" + arrayIndexExpr(ops[0], ops[1]) + "]";
        case beir::OperationKind::Aggregate:
            throw std::runtime_error("rtlgen aggregate operation must be emitted as element assignments");
        case beir::OperationKind::Call:
            throw std::runtime_error("rtlgen cannot lower call operation");
        }
        throw std::runtime_error("rtlgen unsupported operation");
    }

    std::string concatExpr(const std::vector<beir::Operand>& ops) const {
        std::ostringstream os;
        os << "{";
        for (std::size_t i = 0; i < ops.size(); ++i) {
            if (i) os << ", ";
            os << operand(ops[i]);
        }
        os << "}";
        return os.str();
    }

    static int indexWidthForCount(std::size_t count) {
        if (count <= 1) return 1;
        int width = 0;
        std::size_t value = count - 1;
        while (value) {
            ++width;
            value >>= 1;
        }
        return std::max(width, 1);
    }

    std::string arrayIndexExpr(const beir::Operand& array_op, const beir::Operand& index_op) const {
        std::size_t count = static_cast<std::size_t>(flattenedArraySize(array_op.type));
        if (count == 0) return operand(index_op);
        int index_width = indexWidthForCount(count);
        if (widthOf(index_op.type) == index_width) return operand(index_op);
        return std::to_string(index_width) + "'(" + operand(index_op) + ")";
    }

    std::string maskConst(int base_width, int write_width) const {
        beir::Operand::Constant c;
        c.width = base_width;
        c.limbs.assign(static_cast<std::size_t>((base_width + 63) / 64), 0);
        for (int i = 0; i < write_width; ++i) {
            std::size_t limb = static_cast<std::size_t>(i / 64);
            int bit = i % 64;
            if (limb < c.limbs.size()) c.limbs[limb] |= (std::uint64_t{1} << bit);
        }
        return constExpr(c);
    }

    std::string writeSliceExpr(const std::string& base, const std::string& value,
                               int base_width, int lo, int write_width, int value_width) const {
        return writeSliceExpr(base, value, base_width, std::to_string(lo), write_width, value_width);
    }

    std::string writeSliceExpr(const std::string& base, const std::string& value,
                               int base_width, const std::string& index, int write_width,
                               int value_width) const {
        std::string mask = maskConst(base_width, write_width);
        std::string resized_value = resizeExpr(value, value_width, base_width, false);
        return "((" + base + " & ~(" + mask + " << " + index + ")) | " +
               "((" + resized_value + " & " + mask + ") << " + index + "))";
    }
};

} // namespace

std::string emitSystemVerilog(const beir::Program& program) {
    return Emitter(program).emit();
}

std::string emitSystemVerilog(const PredicateProgram& source) {
    return emitSystemVerilog(beir::buildProgram(source));
}

} // namespace pred::rtlgen
