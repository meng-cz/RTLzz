#include "backend/rtlgen.hpp"

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

static int widthOf(const TypeInfo& type) {
    return type.width > 0 ? type.width : 1;
}

static int flattenedArraySize(const TypeInfo& type) {
    if (!type.array_dims.empty()) {
        int size = 1;
        for (int dim : type.array_dims) size *= dim;
        return size;
    }
    return type.array_size > 0 ? type.array_size : 0;
}

static std::vector<int> arrayDims(const TypeInfo& type) {
    if (!type.array_dims.empty()) return type.array_dims;
    if (type.array_size > 0) return {type.array_size};
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
        "endfunction", "signed", "unsigned", "localparam"
    };
    if (keywords.count(out)) out += "_";
    return out;
}

static std::string packedRange(const TypeInfo& type) {
    int width = widthOf(type);
    if (width <= 1) return "";
    return "[" + std::to_string(width - 1) + ":0] ";
}

static std::string signedText(const TypeInfo& type) {
    return type.is_signed ? "signed " : "";
}

static std::string unpackedDims(const TypeInfo& type) {
    std::ostringstream os;
    for (int dim : arrayDims(type)) {
        os << " [0:" << (dim - 1) << "]";
    }
    return os.str();
}

static std::string logicType(const TypeInfo& type) {
    return "logic " + signedText(type) + packedRange(type);
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

static std::string zeroFor(const TypeInfo& type) {
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

class Emitter {
public:
    explicit Emitter(const beir::Program& program) : program_(program) {
        for (const auto& signal : program_.signals) {
            name_to_node_[signal.name] = signal.id;
        }
        for (const auto& aggregate : program_.aggregates) {
            aggregates_by_name_[aggregate.name] = &aggregate;
        }
        for (const auto& table : program_.lookup_tables) {
            lookup_tables_by_name_[table.name] = &table;
        }
    }

    std::string emit() {
        std::ostringstream os;
        os << "`timescale 1ns/1ps\n\n";
        emitModuleHeader(os);
        emitLookupArrays(os);
        emitAggregateArrays(os);
        emitSignalDecls(os);
        emitPortElementConnections(os);
        emitAggregateConnections(os);
        emitSignalAssignments(os);
        os << "endmodule\n";
        return os.str();
    }

private:
    const beir::Program& program_;
    std::unordered_map<std::string, NodeId> name_to_node_;
    std::unordered_map<std::string, const beir::Aggregate*> aggregates_by_name_;
    std::unordered_map<std::string, const beir::LookupTable*> lookup_tables_by_name_;

    std::string sig(NodeId id) const {
        return sanitizeIdentifier(program_.signal(id).name);
    }

    bool isScalarPortSignal(const beir::Signal& signal) const {
        return !signal.port_name.empty() && signal.port_element_index == 0 &&
               signal.name == signal.port_name;
    }

    void emitModuleHeader(std::ostream& os) const {
        os << "module " << sanitizeIdentifier(program_.function_name) << "(\n";
        for (std::size_t i = 0; i < program_.ports.size(); ++i) {
            const auto& port = program_.ports[i];
            os << "    ";
            switch (port.direction) {
            case beir::PortDirection::Input: os << "input "; break;
            case beir::PortDirection::Output: os << "output "; break;
            case beir::PortDirection::InOut: os << "inout "; break;
            case beir::PortDirection::Unknown: os << "input "; break;
            }
            os << logicType(port.type) << sanitizeIdentifier(port.name)
               << unpackedDims(port.type);
            if (i + 1 < program_.ports.size()) os << ",";
            os << "\n";
        }
        os << ");\n\n";
    }

    void emitLookupArrays(std::ostream& os) const {
        for (const auto& table : program_.lookup_tables) {
            if (aggregates_by_name_.count(table.name)) continue;
            os << "    localparam logic [31:0] " << sanitizeIdentifier(table.name)
               << " [0:" << (table.values.empty() ? 0 : table.values.size() - 1) << "] = '{";
            for (std::size_t i = 0; i < table.values.size(); ++i) {
                if (i) os << ", ";
                os << constExpr(parseSmallConst(table.values[i], 32));
            }
            if (table.values.empty()) os << "32'h0";
            os << "};\n";
        }
        if (!program_.lookup_tables.empty()) os << "\n";
    }

    void emitAggregateArrays(std::ostream& os) const {
        for (const auto& aggregate : program_.aggregates) {
            TypeInfo element_type = aggregate.type;
            element_type.is_array = false;
            element_type.array_size = 0;
            element_type.array_dims.clear();
            int count = flattenedArraySize(aggregate.type);
            if (count <= 0) continue;
            os << "    " << logicType(element_type) << sanitizeIdentifier(aggregate.name)
               << " [0:" << (count - 1) << "];\n";
        }
        if (!program_.aggregates.empty()) os << "\n";
    }

    void emitSignalDecls(std::ostream& os) const {
        for (const auto& signal : program_.signals) {
            if (isScalarPortSignal(signal)) continue;
            os << "    " << logicType(signal.type) << sanitizeIdentifier(signal.name) << ";\n";
        }
        if (!program_.signals.empty()) os << "\n";
    }

    void emitPortElementConnections(std::ostream& os) const {
        for (const auto& port : program_.ports) {
            if (port.direction == beir::PortDirection::Output) {
                for (std::size_t i = 0; i < port.element_nodes.size(); ++i) {
                    const auto& signal = program_.signal(port.element_nodes[i]);
                    if (isScalarPortSignal(signal)) continue;
                    os << "    assign " << sanitizeIdentifier(port.name) << "[" << i << "] = "
                       << sig(signal.id) << ";\n";
                }
                continue;
            }
            if (port.type.is_array) {
                for (std::size_t i = 0; i < port.element_nodes.size(); ++i) {
                    os << "    assign " << sig(port.element_nodes[i]) << " = "
                       << sanitizeIdentifier(port.name) << "[" << i << "];\n";
                }
            }
        }
        os << "\n";
    }

    NodeId latestElementNode(const std::string& aggregate_name, std::size_t index) const {
        std::string stem = aggregate_name + "_" + std::to_string(index);
        NodeId best = beir::kInvalidNodeId;
        int best_version = -1;
        auto base_it = name_to_node_.find(stem);
        if (base_it != name_to_node_.end()) best = base_it->second;
        for (const auto& signal : program_.signals) {
            std::string base;
            if (!endsWithNumber(signal.name, base) || base != stem) continue;
            std::string version_text = signal.name.substr(base.size() + 1);
            int version = std::stoi(version_text);
            if (version > best_version) {
                best_version = version;
                best = signal.id;
            }
        }
        if (best == beir::kInvalidNodeId) {
            throw std::runtime_error("rtlgen cannot find aggregate element: " + stem);
        }
        return best;
    }

    bool hasVersionedElement(const std::string& aggregate_name, std::size_t index) const {
        std::string stem = aggregate_name + "_" + std::to_string(index);
        for (const auto& signal : program_.signals) {
            std::string base;
            if (endsWithNumber(signal.name, base) && base == stem) return true;
        }
        return false;
    }

    void emitAggregateConnections(std::ostream& os) const {
        for (const auto& aggregate : program_.aggregates) {
            int count = flattenedArraySize(aggregate.type);
            TypeInfo element_type = aggregate.type;
            element_type.is_array = false;
            element_type.array_size = 0;
            element_type.array_dims.clear();
            auto table_it = lookup_tables_by_name_.find(aggregate.name);
            for (int i = 0; i < count; ++i) {
                std::size_t index = static_cast<std::size_t>(i);
                os << "    assign " << sanitizeIdentifier(aggregate.name) << "[" << i << "] = ";
                if (table_it != lookup_tables_by_name_.end() &&
                    index < table_it->second->values.size() &&
                    !hasVersionedElement(aggregate.name, index)) {
                    os << constExpr(parseSmallConst(table_it->second->values[index],
                                                    widthOf(element_type)));
                } else {
                    NodeId id = latestElementNode(aggregate.name, index);
                    os << sig(id);
                }
                os << ";\n";
            }
        }
        if (!program_.aggregates.empty()) os << "\n";
    }

    void emitSignalAssignments(std::ostream& os) const {
        for (const auto& signal : program_.signals) {
            if (signal.driver) {
                if (signal.driver->kind == beir::OperationKind::PortRead &&
                    signal.port_element_index == 0 && signal.name == signal.port_name) {
                    continue;
                }
                if (signal.driver->kind == beir::OperationKind::PortRead &&
                    signal.port_element_index >= 0) {
                    continue;
                }
                os << "    assign " << sig(signal.id) << " = "
                   << expr(*signal.driver) << ";\n";
                continue;
            }

            std::string base;
            if (endsWithNumber(signal.name, base)) {
                auto it = name_to_node_.find(base);
                if (it != name_to_node_.end()) {
                    os << "    assign " << sig(signal.id) << " = " << sig(it->second) << ";\n";
                    continue;
                }
            }
            if (!isScalarPortSignal(signal)) {
                os << "    assign " << sig(signal.id) << " = " << zeroFor(signal.type) << ";\n";
            }
        }
    }

    std::string operand(const beir::Operand& op) const {
        switch (op.kind) {
        case beir::OperandKind::Symbol:
            return sig(op.node);
        case beir::OperandKind::Literal:
            return constExpr(op.constant);
        case beir::OperandKind::Port:
        case beir::OperandKind::Aggregate:
        case beir::OperandKind::LookupTable:
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

    std::string expr(const beir::Operation& op) const {
        const auto& ops = op.operands;
        auto need = [&](std::size_t count) {
            if (ops.size() < count) throw std::runtime_error("rtlgen malformed operation operands");
        };

        switch (op.kind) {
        case beir::OperationKind::Assign:
            need(1);
            return operand(ops[0]);
        case beir::OperationKind::PortRead:
            need(1);
            if (ops.size() == 1) return operand(ops[0]);
            return operand(ops[0]) + "[" + operand(ops[1]) + "]";
        case beir::OperationKind::Binary:
            need(2);
            return "(" + operand(ops[0]) + " " + svBinaryOp(op.op) + " " + operand(ops[1]) + ")";
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
            return "(" + operand(ops[0]) + " ? " + operand(ops[1]) + " : " + operand(ops[2]) + ")";
        case beir::OperationKind::Slice:
            need(1);
            return "(" + operand(ops[0]) + "[" + std::to_string(op.hi) + ":" +
                   std::to_string(op.lo) + "])";
        case beir::OperationKind::BitSelect:
            need(1);
            return "(" + operand(ops[0]) + "[" + std::to_string(op.bit) + "])";
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
            return "((" + operand(ops[0]) + " >> " + operand(ops[1]) + ") & 1'b1)";
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
            return operand(ops[0]) + "[" + operand(ops[1]) + "]";
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
