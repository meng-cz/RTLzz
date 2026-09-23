#include "backend/circt_bridge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pred::circt {
namespace {

using beir::NodeId;

int widthOf(const beir::ValueType& type) { return type.width > 0 ? type.width : 1; }

int unsignedWidthForValue(unsigned value) {
    int width = 1;
    while (value > 1) {
        ++width;
        value >>= 1;
    }
    return width;
}

std::string sanitize(std::string text) {
    for (char& ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '_')) ch = '_';
    }
    if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front()))) text.insert(text.begin(), '_');
    return text;
}

std::string typeOf(int width) { return "i" + std::to_string(std::max(1, width)); }

std::string shellQuote(const std::string& text) {
    std::string out = "'";
    for (char ch : text) out += ch == '\'' ? "'\\''" : std::string(1, ch);
    return out + "'";
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read '" + path.string() + "'");
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path);
    if (!output) throw std::runtime_error("cannot write '" + path.string() + "'");
    output << text;
    if (!output) throw std::runtime_error("failed while writing '" + path.string() + "'");
}

std::string constantDigits(const beir::Operand::Constant& constant) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    bool started = false;
    for (std::size_t index = constant.limbs.size(); index > 0; --index) {
        const auto limb = constant.limbs[index - 1];
        if (!started) {
            if (limb == 0 && index > 1) continue;
            out << limb;
            started = true;
        } else {
            out << std::setw(16) << limb;
        }
    }
    return started ? out.str() : "0";
}

class Lowerer {
public:
    Lowerer(const beir::Program& program,
            bool module_body,
            const std::vector<std::pair<std::string, std::string>>& bindings)
        : program_(program), module_body_(module_body) {
        if (!program.function_name.empty()) module_name_ = sanitize(program.function_name);
        for (const auto& port : program_.ports)
            has_array_ports_ = has_array_ports_ || port.type.isArray();
        if (has_array_ports_ && !module_body_) impl_module_name_ = module_name_ + "__circt_impl";
        else impl_module_name_ = module_name_;
        for (const auto& [port, expression] : bindings) {
            if (!bindings_.emplace(port, expression).second)
                throw std::runtime_error("CIRCT bridge received duplicate port binding '" + port + "'");
        }
        collectPorts();
    }

    std::string moduleName() const { return impl_module_name_; }
    std::string lower() {
        std::ostringstream out;
        out << "hw.module @" << impl_module_name_ << "(";
        bool first = true;
        for (const auto& port : inputs_) appendPort(out, first, "in", port.name, port.width);
        for (const auto& port : outputs_) appendPort(out, first, "out", port.name, port.width);
        out << ") {\n";
        std::vector<std::string> output_values;
        output_values.reserve(outputs_.size());
        for (const auto& output : outputs_) output_values.push_back(valueForNode(output.node, out));
        out << "  hw.output";
        for (std::size_t index = 0; index < output_values.size(); ++index)
            out << (index ? ", " : " ") << output_values[index];
        if (!outputs_.empty()) {
            out << " : ";
            for (std::size_t i = 0; i < outputs_.size(); ++i) {
                if (i) out << ", ";
                out << typeOf(outputs_[i].width);
            }
        }
        out << "\n}\n";
        return out.str();
    }

    std::string moduleBody(const std::string& exported) const {
        const std::string module = exportedModule(exported);
        const std::size_t header_end = module.find(");");
        const std::size_t end = module.rfind("endmodule");
        if (header_end == std::string::npos || end == std::string::npos)
            throw std::runtime_error("CIRCT exporter produced an unrecognizable Verilog module wrapper");
        std::ostringstream body;
        emitPortPrelude(body);
        body << module.substr(header_end + 2, end - (header_end + 2));
        return body.str();
    }

    std::string moduleVerilog(const std::string& exported) const {
        const std::string implementation = exportedModule(exported);
        return has_array_ports_ ? implementation + "\n" + arrayPortWrapper() : implementation;
    }

private:
    std::string exportedModule(const std::string& exported) const {
        const std::string marker = "module " + impl_module_name_;
        const std::size_t begin = exported.find(marker);
        if (begin == std::string::npos) throw std::runtime_error("CIRCT exporter did not emit module '" + impl_module_name_ + "'");
        const std::size_t header_end = exported.find(");", begin);
        const std::size_t end = exported.find("endmodule", header_end == std::string::npos ? begin : header_end);
        if (header_end == std::string::npos || end == std::string::npos)
            throw std::runtime_error("CIRCT exporter produced an unrecognizable Verilog module wrapper");
        return exported.substr(begin, end + std::string("endmodule").size() - begin) + "\n";
    }

    struct PortValue { NodeId node; std::string name; int width; std::string binding; bool input; };
    const beir::Program& program_;
    bool module_body_;
    std::unordered_map<std::string, std::string> bindings_;
    std::string module_name_ = "rtlzz_circt_logic";
    std::string impl_module_name_ = "rtlzz_circt_logic";
    bool has_array_ports_ = false;
    std::vector<PortValue> inputs_;
    std::vector<PortValue> outputs_;
    std::unordered_map<NodeId, std::string> values_;
    std::unordered_map<NodeId, std::vector<beir::Operand>> arrays_;
    std::unordered_set<NodeId> lowering_;
    unsigned temporary_ = 0;

    static void appendPort(std::ostream& out, bool& first, const char* direction,
                           const std::string& name, int width) {
        if (!first) out << ", ";
        first = false;
        out << direction << " " << (std::string(direction) == "in" ? "%" : "") << name
            << " : " << typeOf(width);
    }

    std::string portName(const beir::Port& port, std::size_t element) const {
        if (port.element_nodes.size() == 1) return sanitize(port.name);
        // CIRCT currently receives flattened scalar ports. Keep names free of
        // BEIR's internal "__idx_" marker because the SV exporter canonicalizes
        // that marker differently for input and output ports.
        return sanitize(port.name) + "_" + std::to_string(element);
    }

    std::string bindingFor(const beir::Port& port, std::size_t element) const {
        auto found = bindings_.find(port.name);
        if (module_body_ && found == bindings_.end())
            throw std::runtime_error("CIRCT bridge missing module-body binding for port '" + port.name + "'");
        const std::string base = found == bindings_.end() ? sanitize(port.name) : found->second;
        return port.element_nodes.size() == 1 ? base : base + "[" + std::to_string(element) + "]";
    }

    void collectPorts() {
        std::unordered_set<NodeId> seen_inputs;
        std::unordered_set<NodeId> seen_outputs;
        std::unordered_set<std::string> names;
        for (const auto& port : program_.ports) {
            if (port.direction == beir::PortDirection::Unknown) continue;
            for (std::size_t index = 0; index < port.element_nodes.size(); ++index) {
                const NodeId node = port.element_nodes[index];
                const auto& signal = program_.signal(node);
                if (signal.type.isArray())
                    throw std::runtime_error("CIRCT bridge does not support an unflattened array port element");
                const std::string name = portName(port, index);
                if (!names.insert(name).second)
                    throw std::runtime_error("CIRCT bridge port name collision for '" + name + "'");
                PortValue value{node, name, widthOf(signal.type), bindingFor(port, index),
                                port.direction == beir::PortDirection::Input};
                if (value.input) {
                    if (seen_inputs.insert(node).second) {
                        values_[node] = "%" + name;
                        inputs_.push_back(std::move(value));
                    }
                } else if (seen_outputs.insert(node).second) {
                    outputs_.push_back(std::move(value));
                }
            }
        }
        if (outputs_.empty()) throw std::runtime_error("CIRCT bridge requires at least one BEIR output port");
    }

    std::string fresh() { return "%t" + std::to_string(temporary_++); }
    std::string emitConstant(std::ostream& out, const beir::Operand::Constant& constant, int width) {
        beir::Operand::Constant normalized = constant;
        normalized.width = width;
        normalized.limbs.resize(static_cast<std::size_t>((width + 63) / 64), 0);
        if (!normalized.limbs.empty() && width % 64 != 0) {
            normalized.limbs.back() &= (std::uint64_t{1} << (width % 64)) - 1;
        }
        const std::string result = fresh();
        out << "  " << result << " = hw.constant 0x" << constantDigits(normalized)
            << " : " << typeOf(width) << "\n";
        return result;
    }
    std::string emitUnsignedConstant(std::ostream& out, std::uint64_t value, int width) {
        beir::Operand::Constant constant;
        constant.width = width;
        constant.limbs.assign(static_cast<std::size_t>((width + 63) / 64), 0);
        if (!constant.limbs.empty()) constant.limbs[0] = value;
        return emitConstant(out, constant, width);
    }
    std::string emitOnes(std::ostream& out, int width) {
        beir::Operand::Constant constant;
        constant.width = width;
        constant.limbs.assign(static_cast<std::size_t>((width + 63) / 64), ~std::uint64_t{0});
        return emitConstant(out, constant, width);
    }

    std::string value(const beir::Operand& operand, std::ostream& out) {
        if (operand.kind == beir::OperandKind::Literal) return emitConstant(out, operand.constant, widthOf(operand.type));
        if (operand.kind != beir::OperandKind::Symbol)
            throw std::runtime_error("CIRCT bridge does not support a raw BEIR port operand");
        return valueForNode(operand.node, out);
    }
    std::string valueForNode(NodeId node, std::ostream& out) {
        auto found = values_.find(node);
        if (found == values_.end() && arrays_.count(node) == 0) {
            const auto* signal = program_.findSignal(node);
            if (signal) lowerSignal(*signal, out);
            found = values_.find(node);
        }
        if (found == values_.end()) {
            const auto* signal = program_.findSignal(node);
            throw std::runtime_error("CIRCT bridge encountered unresolved BEIR signal #" + std::to_string(node) +
                                     (signal ? " ('" + signal->name + "')" : ""));
        }
        return found->second;
    }

    std::string resize(const std::string& source, int from, int to, bool sign, std::ostream& out) {
        from = std::max(1, from); to = std::max(1, to);
        if (from == to) return source;
        if (from > to) {
            const std::string result = fresh();
            out << "  " << result << " = comb.extract " << source << " from 0 : ("
                << typeOf(from) << ") -> " << typeOf(to) << "\n";
            return result;
        }
        const int pad = to - from;
        std::string prefix;
        if (!sign) prefix = emitUnsignedConstant(out, 0, pad);
        else {
            const std::string bit = fresh();
            out << "  " << bit << " = comb.extract " << source << " from " << (from - 1)
                << " : (" << typeOf(from) << ") -> i1\n";
            prefix = bit;
            for (int count = 1; count < pad; ++count) {
                const std::string repeated = fresh();
                out << "  " << repeated << " = comb.concat " << prefix << ", " << bit
                    << " : " << typeOf(count) << ", i1\n";
                prefix = repeated;
            }
        }
        const std::string result = fresh();
        out << "  " << result << " = comb.concat " << prefix << ", " << source << " : "
            << typeOf(pad) << ", " << typeOf(from) << "\n";
        return result;
    }

    std::string signedTruncate(const std::string& source, int from, int to, std::ostream& out) {
        from = std::max(1, from);
        to = std::max(1, to);
        if (from <= to) return resize(source, from, to, true, out);
        const std::string sign = fresh();
        out << "  " << sign << " = comb.extract " << source << " from " << (from - 1)
            << " : (" << typeOf(from) << ") -> i1\n";
        if (to == 1) return sign;
        const std::string low = fresh();
        out << "  " << low << " = comb.extract " << source << " from 0 : ("
            << typeOf(from) << ") -> " << typeOf(to - 1) << "\n";
        const std::string result = fresh();
        out << "  " << result << " = comb.concat " << sign << ", " << low
            << " : i1, " << typeOf(to - 1) << "\n";
        return result;
    }

    std::string binary(beir::OpCode code, std::string lhs, std::string rhs, int width,
                       std::ostream& out) {
        const char* op = nullptr;
        switch (code) {
        case beir::OpCode::Add: op = "comb.add"; break;
        case beir::OpCode::Sub: op = "comb.sub"; break;
        case beir::OpCode::Mul: op = "comb.mul"; break;
        case beir::OpCode::Div: op = "comb.divu"; break;
        case beir::OpCode::Mod: op = "comb.modu"; break;
        case beir::OpCode::BitAnd: case beir::OpCode::LogicAnd: op = "comb.and"; break;
        case beir::OpCode::BitOr: case beir::OpCode::LogicOr: op = "comb.or"; break;
        case beir::OpCode::BitXor: op = "comb.xor"; break;
        case beir::OpCode::Shl: op = "comb.shl"; break;
        case beir::OpCode::Shr: op = "comb.shru"; break;
        default: throw std::runtime_error("CIRCT bridge unsupported binary opcode");
        }
        const std::string result = fresh();
        out << "  " << result << " = " << op << " " << lhs << ", " << rhs
            << " : " << typeOf(width) << "\n";
        return result;
    }

    std::string compare(beir::OpCode code, const beir::Operand& lhs_op,
                        const beir::Operand& rhs_op, std::ostream& out) {
        const int width = std::max(widthOf(lhs_op.type), widthOf(rhs_op.type));
        const bool signed_context = lhs_op.signed_view || rhs_op.signed_view ||
                                    lhs_op.constant.signed_view || rhs_op.constant.signed_view;
        std::string lhs = resize(value(lhs_op, out), widthOf(lhs_op.type), width,
                                 signed_context && (lhs_op.signed_view || lhs_op.constant.signed_view), out);
        std::string rhs = resize(value(rhs_op, out), widthOf(rhs_op.type), width,
                                 signed_context && (rhs_op.signed_view || rhs_op.constant.signed_view), out);
        const char* predicate = nullptr;
        switch (code) {
        case beir::OpCode::Eq: predicate = "eq"; break;
        case beir::OpCode::Ne: predicate = "ne"; break;
        case beir::OpCode::Lt: predicate = signed_context ? "slt" : "ult"; break;
        case beir::OpCode::Le: predicate = signed_context ? "sle" : "ule"; break;
        case beir::OpCode::Gt: predicate = signed_context ? "sgt" : "ugt"; break;
        case beir::OpCode::Ge: predicate = signed_context ? "sge" : "uge"; break;
        default: throw std::runtime_error("CIRCT bridge unsupported comparison opcode");
        }
        const std::string result = fresh();
        out << "  " << result << " = comb.icmp " << predicate << " " << lhs << ", " << rhs
            << " : " << typeOf(width) << "\n";
        return result;
    }

    std::string mux(std::string condition, std::string yes, std::string no, int width, std::ostream& out) {
        const std::string result = fresh();
        out << "  " << result << " = comb.mux " << condition << ", " << yes << ", " << no
            << " : " << typeOf(width) << "\n";
        return result;
    }

    std::vector<beir::Operand> arrayElements(beir::Operand array) const {
        for (std::size_t depth = 0; depth <= program_.signals.size(); ++depth) {
            if (array.kind != beir::OperandKind::Symbol) break;
            auto found = arrays_.find(array.node);
            if (found != arrays_.end()) return found->second;
            const auto* signal = program_.findSignal(array.node);
            if (!signal || !signal->driver) break;
            const auto& op = *signal->driver;
            if (op.kind == beir::OperationKind::Aggregate) return op.operands;
            if (op.kind != beir::OperationKind::Assign || op.operands.size() != 1) break;
            array = op.operands[0];
        }
        throw std::runtime_error("CIRCT bridge Lookup requires an Aggregate-backed signal-level array");
    }

    std::string lookup(const beir::Operation& op, std::ostream& out) {
        if (op.operands.size() != 2) throw std::runtime_error("CIRCT bridge malformed Lookup");
        const auto entries = arrayElements(op.operands[0]);
        const int result_width = widthOf(op.type);
        std::string result = emitUnsignedConstant(out, 0, result_width);
        for (std::size_t index = entries.size(); index > 0; --index) {
            const std::string condition = compare(beir::OpCode::Eq, op.operands[1],
                                                  literal(index - 1, widthOf(op.operands[1].type)), out);
            const auto& entry = entries[index - 1];
            const std::string selected = resize(value(entry, out), widthOf(entry.type), result_width, false, out);
            result = mux(condition, selected, result, result_width, out);
        }
        return result;
    }

    static beir::Operand literal(std::uint64_t value, int width) {
        beir::Operand operand;
        operand.kind = beir::OperandKind::Literal;
        operand.type.width = width;
        operand.constant.width = width;
        operand.constant.limbs.assign(static_cast<std::size_t>((width + 63) / 64), 0);
        if (!operand.constant.limbs.empty()) operand.constant.limbs[0] = value;
        return operand;
    }

    std::string staticWrite(const std::string& base, const std::string& data, int base_width,
                            int lo, int data_width, std::ostream& out) {
        if (lo < 0 || data_width <= 0 || lo + data_width > base_width)
            throw std::runtime_error("CIRCT bridge write range is out of bounds");
        std::vector<std::pair<std::string, int>> pieces;
        if (lo + data_width < base_width) {
            const int width = base_width - lo - data_width;
            const std::string high = fresh();
            out << "  " << high << " = comb.extract " << base << " from " << (lo + data_width)
                << " : (" << typeOf(base_width) << ") -> " << typeOf(width) << "\n";
            pieces.push_back({high, width});
        }
        pieces.push_back({resize(data, data_width, data_width, false, out), data_width});
        if (lo > 0) {
            const std::string low = fresh();
            out << "  " << low << " = comb.extract " << base << " from 0 : ("
                << typeOf(base_width) << ") -> " << typeOf(lo) << "\n";
            pieces.push_back({low, lo});
        }
        std::string result = pieces.front().first;
        int result_width = pieces.front().second;
        for (std::size_t index = 1; index < pieces.size(); ++index) {
            const std::string joined = fresh();
            out << "  " << joined << " = comb.concat " << result << ", " << pieces[index].first
                << " : " << typeOf(result_width) << ", " << typeOf(pieces[index].second) << "\n";
            result = joined;
            result_width += pieces[index].second;
        }
        return result;
    }

    std::string dynamicWrite(const beir::Operation& op, std::ostream& out) {
        if (op.operands.size() != 3) throw std::runtime_error("CIRCT bridge malformed dynamic write");
        const int base_width = widthOf(op.operands[0].type);
        const int write_width = op.kind == beir::OperationKind::DynamicWriteBit ? 1 : widthOf(op.operands[2].type);
        std::string base = resize(value(op.operands[0], out), base_width, widthOf(op.type), false, out);
        std::string result = base;
        const std::string data = resize(value(op.operands[2], out), widthOf(op.operands[2].type), write_width, false, out);
        for (int index = base_width - write_width; index >= 0; --index) {
            const std::string condition = compare(beir::OpCode::Eq, op.operands[1],
                                                  literal(static_cast<std::uint64_t>(index), widthOf(op.operands[1].type)), out);
            const std::string written = staticWrite(base, data, base_width, index, write_width, out);
            result = mux(condition, written, result, base_width, out);
        }
        return result;
    }

    std::string dynamicSelect(const beir::Operation& op, std::ostream& out) {
        if (op.operands.size() != 2) throw std::runtime_error("CIRCT bridge malformed dynamic select");
        const int base_width = widthOf(op.operands[0].type);
        const int select_width = op.kind == beir::OperationKind::DynamicBitSelect ? 1 : widthOf(op.type);
        const std::string base = value(op.operands[0], out);
        std::string result = emitUnsignedConstant(out, 0, select_width);
        for (int index = base_width - select_width; index >= 0; --index) {
            const std::string condition = compare(beir::OpCode::Eq, op.operands[1],
                                                  literal(static_cast<std::uint64_t>(index), widthOf(op.operands[1].type)), out);
            const std::string selected = fresh();
            out << "  " << selected << " = comb.extract " << base << " from " << index << " : ("
                << typeOf(base_width) << ") -> " << typeOf(select_width) << "\n";
            result = mux(condition, selected, result, select_width, out);
        }
        return result;
    }

    std::string reduce(const beir::Operation& op, std::ostream& out) {
        if (op.operands.size() != 1) throw std::runtime_error("CIRCT bridge malformed reduction");
        const int input_width = widthOf(op.operands[0].type);
        const std::string input = value(op.operands[0], out);
        std::string result;
        for (int bit = 0; bit < input_width; ++bit) {
            const std::string selected = fresh();
            out << "  " << selected << " = comb.extract " << input << " from " << bit
                << " : (" << typeOf(input_width) << ") -> i1\n";
            if (bit == 0) result = selected;
            else result = binary(op.kind == beir::OperationKind::ReduceAnd ? beir::OpCode::BitAnd :
                                 op.kind == beir::OperationKind::ReduceXor ? beir::OpCode::BitXor : beir::OpCode::BitOr,
                                 result, selected, 1, out);
        }
        return result;
    }

    std::string lowerOperation(const beir::Operation& op, std::ostream& out) {
        const auto need = [&](std::size_t count) {
            if (op.operands.size() < count) throw std::runtime_error("CIRCT bridge malformed BEIR operation");
        };
        const int result_width = widthOf(op.type);
        switch (op.kind) {
        case beir::OperationKind::Assign:
        case beir::OperationKind::Cast:
        case beir::OperationKind::ZExt:
        case beir::OperationKind::Trunc:
            need(1);
            {
                const int source_width = widthOf(op.operands[0].type);
                const std::string source = value(op.operands[0], out);
                if (op.signed_truncation && source_width > result_width)
                    return signedTruncate(source, source_width, result_width, out);
                return resize(source, source_width, result_width, false, out);
            }
        case beir::OperationKind::SExt:
            need(1); return resize(value(op.operands[0], out), widthOf(op.operands[0].type), result_width, true, out);
        case beir::OperationKind::AddCarry: {
            need(3);
            std::string lhs = resize(value(op.operands[0], out), widthOf(op.operands[0].type), result_width, op.operands[0].signed_view, out);
            std::string rhs = resize(value(op.operands[1], out), widthOf(op.operands[1].type), result_width, op.operands[1].signed_view, out);
            std::string carry = resize(value(op.operands[2], out), widthOf(op.operands[2].type), result_width, false, out);
            return binary(beir::OpCode::Add, binary(beir::OpCode::Add, lhs, rhs, result_width, out), carry, result_width, out);
        }
        case beir::OperationKind::Binary: {
            need(2);
            if (op.op == beir::OpCode::Eq || op.op == beir::OpCode::Ne || op.op == beir::OpCode::Lt ||
                op.op == beir::OpCode::Le || op.op == beir::OpCode::Gt || op.op == beir::OpCode::Ge)
                return compare(op.op, op.operands[0], op.operands[1], out);
            const bool signed_context = op.operands[0].signed_view || op.operands[1].signed_view ||
                                        op.operands[0].constant.signed_view || op.operands[1].constant.signed_view;
            const int lhs_width = widthOf(op.operands[0].type);
            if (op.op == beir::OpCode::Shl || op.op == beir::OpCode::Shr) {
                // BEIR defines shifts at the source width, then applies the
                // result-width conversion.  In particular, a narrowed signed
                // right shift must retain the original sign bit while shifting.
                const bool arithmetic = op.op == beir::OpCode::Shr &&
                                        (op.operands[0].signed_view || op.operands[0].constant.signed_view);
                const std::string source = value(op.operands[0], out);
                const std::string amount = resize(value(op.operands[1], out),
                                                  widthOf(op.operands[1].type), lhs_width, false, out);
                const std::string shifted = fresh();
                out << "  " << shifted << " = "
                    << (op.op == beir::OpCode::Shl ? "comb.shl" : (arithmetic ? "comb.shrs" : "comb.shru"))
                    << " " << source << ", " << amount << " : " << typeOf(lhs_width) << "\n";
                std::string result = resize(shifted, lhs_width, result_width, false, out);

                // Int<W> totalizes oversized shifts to zero.  comb.shrs would
                // otherwise sign-fill a negative value, so preserve the BEIR
                // guard explicitly for every shift direction.
                const int amount_width = widthOf(op.operands[1].type);
                if (amount_width >= unsignedWidthForValue(static_cast<unsigned>(lhs_width))) {
                    const std::string too_large = compare(beir::OpCode::Ge, op.operands[1],
                                                          literal(static_cast<std::uint64_t>(lhs_width), amount_width), out);
                    result = mux(too_large, emitUnsignedConstant(out, 0, result_width), result, result_width, out);
                }
                return result;
            }
            std::string lhs = resize(value(op.operands[0], out), lhs_width, result_width,
                                     signed_context && (op.operands[0].signed_view || op.operands[0].constant.signed_view), out);
            std::string rhs = resize(value(op.operands[1], out), widthOf(op.operands[1].type), result_width,
                                     signed_context && (op.operands[1].signed_view || op.operands[1].constant.signed_view), out);
            return binary(op.op, lhs, rhs, result_width, out);
        }
        case beir::OperationKind::Unary: {
            need(1);
            const std::string input = resize(value(op.operands[0], out), widthOf(op.operands[0].type), result_width, false, out);
            if (op.op == beir::OpCode::Neg) return binary(beir::OpCode::Sub, emitUnsignedConstant(out, 0, result_width), input, result_width, out);
            if (op.op == beir::OpCode::BitNot) return binary(beir::OpCode::BitXor, input, emitOnes(out, result_width), result_width, out);
            if (op.op == beir::OpCode::LogicNot) return binary(beir::OpCode::BitXor, reduce(beir::Operation{beir::OperationKind::ReduceOr, beir::OpCode::None, {op.operands[0]}, {1, {}}}, out), emitUnsignedConstant(out, 1, 1), 1, out);
            throw std::runtime_error("CIRCT bridge unsupported unary opcode");
        }
        case beir::OperationKind::Ite:
            need(3); return mux(value(op.operands[0], out), resize(value(op.operands[1], out), widthOf(op.operands[1].type), result_width, false, out), resize(value(op.operands[2], out), widthOf(op.operands[2].type), result_width, false, out), result_width, out);
        case beir::OperationKind::Case: {
            if (!beir::hasValidCaseShape(op)) throw std::runtime_error("CIRCT bridge malformed Case operation");
            std::string result = resize(value(op.operands.back(), out), widthOf(op.operands.back().type), result_width, false, out);
            for (std::size_t branch = beir::caseBranchCount(op); branch > 0; --branch) {
                const auto& condition = op.operands[(branch - 1) * 2];
                const auto& selected = op.operands[(branch - 1) * 2 + 1];
                result = mux(value(condition, out), resize(value(selected, out), widthOf(selected.type), result_width, false, out), result, result_width, out);
            }
            return result;
        }
        case beir::OperationKind::Slice: {
            need(1);
            const std::string input = value(op.operands[0], out);
            const std::string result = fresh();
            out << "  " << result << " = comb.extract " << input << " from " << op.lo
                << " : (" << typeOf(widthOf(op.operands[0].type)) << ") -> " << typeOf(result_width) << "\n";
            return result;
        }
        case beir::OperationKind::BitSelect: {
            need(1);
            const std::string input = value(op.operands[0], out);
            const std::string result = fresh();
            out << "  " << result << " = comb.extract " << input << " from " << op.bit
                << " : (" << typeOf(widthOf(op.operands[0].type)) << ") -> i1\n";
            return result;
        }
        case beir::OperationKind::WriteSlice:
            need(2); return staticWrite(value(op.operands[0], out), value(op.operands[1], out), widthOf(op.operands[0].type), op.lo, op.hi - op.lo + 1, out);
        case beir::OperationKind::WriteBit:
            need(2); return staticWrite(value(op.operands[0], out), value(op.operands[1], out), widthOf(op.operands[0].type), op.bit, 1, out);
        case beir::OperationKind::DynamicBitSelect:
        case beir::OperationKind::DynamicSlice: return dynamicSelect(op, out);
        case beir::OperationKind::DynamicWriteSlice:
        case beir::OperationKind::DynamicWriteBit: return dynamicWrite(op, out);
        case beir::OperationKind::Concat: {
            if (op.operands.empty()) throw std::runtime_error("CIRCT bridge empty Concat");
            std::string result = value(op.operands[0], out);
            int current_width = widthOf(op.operands[0].type);
            for (std::size_t index = 1; index < op.operands.size(); ++index) {
                const std::string next = value(op.operands[index], out);
                const int next_width = widthOf(op.operands[index].type);
                const std::string joined = fresh();
                out << "  " << joined << " = comb.concat " << result << ", " << next << " : "
                    << typeOf(current_width) << ", " << typeOf(next_width) << "\n";
                result = joined; current_width += next_width;
            }
            return resize(result, current_width, result_width, false, out);
        }
        case beir::OperationKind::Repeat: {
            need(1);
            std::string result = value(op.operands[0], out);
            int current_width = widthOf(op.operands[0].type);
            for (int count = 1; count < op.times; ++count) {
                const std::string repeated = value(op.operands[0], out);
                const std::string joined = fresh();
                out << "  " << joined << " = comb.concat " << result << ", " << repeated
                    << " : " << typeOf(current_width) << ", " << typeOf(widthOf(op.operands[0].type)) << "\n";
                result = joined; current_width += widthOf(op.operands[0].type);
            }
            return resize(result, current_width, result_width, false, out);
        }
        case beir::OperationKind::ReduceOr:
        case beir::OperationKind::ReduceAnd:
        case beir::OperationKind::ReduceXor: return reduce(op, out);
        case beir::OperationKind::Lookup:
        case beir::OperationKind::ArrayAccess: return lookup(op, out);
        case beir::OperationKind::PortRead:
            need(1); return valueForNode(op.operands[0].node, out);
        case beir::OperationKind::Aggregate:
        case beir::OperationKind::Call:
            throw std::runtime_error("CIRCT bridge operation has no scalar result");
        }
        throw std::runtime_error("CIRCT bridge unsupported BEIR operation");
    }

    void lowerSignal(const beir::Signal& signal, std::ostream& out) {
        if (values_.count(signal.id)) return;
        if (!signal.driver) {
            std::string base;
            const std::size_t pos = signal.name.rfind('_');
            if (pos != std::string::npos && pos + 1 < signal.name.size() &&
                std::all_of(signal.name.begin() + static_cast<std::ptrdiff_t>(pos + 1), signal.name.end(), ::isdigit)) {
                base = signal.name.substr(0, pos);
                for (const auto& candidate : program_.signals) {
                    if (candidate.name == base) { values_[signal.id] = valueForNode(candidate.id, out); return; }
                }
            }
            throw std::runtime_error("CIRCT bridge cannot lower undriven BEIR signal '" + signal.name + "'");
        }
        const auto& op = *signal.driver;
        if (op.kind == beir::OperationKind::PortRead) {
            if (!values_.count(signal.id))
                throw std::runtime_error("CIRCT bridge PortRead signal is not a declared input: '" + signal.name + "'");
            return;
        }
        if (op.kind == beir::OperationKind::Aggregate) {
            arrays_[signal.id] = op.operands;
            return;
        }
        if (signal.type.isArray()) throw std::runtime_error("CIRCT bridge only supports Aggregate signal arrays");
        if (!lowering_.insert(signal.id).second)
            throw std::runtime_error("CIRCT bridge found a cyclic BEIR dependency at '" + signal.name + "'");
        try {
            values_[signal.id] = lowerOperation(op, out);
            lowering_.erase(signal.id);
        } catch (...) {
            lowering_.erase(signal.id);
            throw;
        }
    }

    void emitPortPrelude(std::ostream& out) const {
        for (const auto& port : inputs_) {
            if (port.name == port.binding) continue;
            out << "  logic " << (port.width > 1 ? "[" + std::to_string(port.width - 1) + ":0] " : "")
                << port.name << ";\n  assign " << port.name << " = " << port.binding << ";\n";
        }
        for (const auto& port : outputs_) {
            if (port.name == port.binding) continue;
            out << "  logic " << (port.width > 1 ? "[" + std::to_string(port.width - 1) + ":0] " : "")
                << port.name << ";\n  assign " << port.binding << " = " << port.name << ";\n";
        }
    }

    std::string arrayPortRef(const beir::Port& port, std::size_t flat_index) const {
        std::string ref = sanitize(port.name);
        std::size_t remainder = flat_index;
        std::vector<int> indices(port.type.array_dims.size(), 0);
        for (std::size_t i = port.type.array_dims.size(); i > 0; --i) {
            const int dimension = port.type.array_dims[i - 1];
            if (dimension <= 0) throw std::runtime_error("CIRCT bridge encountered an invalid array port dimension");
            indices[i - 1] = static_cast<int>(remainder % static_cast<std::size_t>(dimension));
            remainder /= static_cast<std::size_t>(dimension);
        }
        for (int index : indices) ref += "[" + std::to_string(index) + "]";
        return ref;
    }

    void emitWrapperPort(std::ostream& out, bool& first, const beir::Port& port) const {
        if (port.direction == beir::PortDirection::Unknown) return;
        if (!first) out << ",\n";
        first = false;
        out << "    " << (port.direction == beir::PortDirection::Input ? "input " : "output ")
            << "logic ";
        if (widthOf(port.type) > 1) out << "[" << (widthOf(port.type) - 1) << ":0] ";
        out << sanitize(port.name);
        for (int dimension : port.type.array_dims) {
            if (dimension <= 0) throw std::runtime_error("CIRCT bridge encountered an invalid array port dimension");
            out << " [0:" << (dimension - 1) << "]";
        }
    }

    std::string arrayPortWrapper() const {
        std::ostringstream out;
        out << "module " << module_name_ << "(\n";
        bool first = true;
        for (const auto& port : program_.ports) emitWrapperPort(out, first, port);
        out << "\n);\n";
        out << "  " << impl_module_name_ << " u_circt_impl (\n";
        bool first_connection = true;
        for (const auto& port : program_.ports) {
            if (port.direction == beir::PortDirection::Unknown) continue;
            for (std::size_t index = 0; index < port.element_nodes.size(); ++index) {
                if (!first_connection) out << ",\n";
                first_connection = false;
                const std::string inner = portName(port, index);
                const std::string outer = port.type.isArray()
                    ? arrayPortRef(port, index) : sanitize(port.name);
                out << "    ." << inner << "(" << outer << ")";
            }
        }
        out << "\n  );\nendmodule\n";
        return out.str();
    }
};

std::filesystem::path temporaryDirectory() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#ifndef _WIN32
    const auto pid = static_cast<unsigned long>(getpid());
#else
    const auto pid = 0UL;
#endif
    const auto path = std::filesystem::temp_directory_path() /
        ("rtlzz-circt-" + std::to_string(pid) + "-" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

} // namespace

Result emitSystemVerilog(const beir::Program& program,
                         bool module_body,
                         const std::vector<std::pair<std::string, std::string>>& port_bindings,
                         bool retain_intermediates) {
    Result result;
    std::filesystem::path directory;
    try {
        Lowerer lowerer(program, module_body, port_bindings);
        directory = temporaryDirectory();
        const auto input = directory / "input.mlir";
        const auto output = directory / "output.txt";
        const auto error = directory / "circt.stderr";
        writeFile(input, lowerer.lower());
        const std::string command =
            "circt-opt --comb-assume-two-valued --canonicalize --cse "
            "--comb-int-range-narrowing --canonicalize --cse "
            "--comb-overflow-annotating --comb-balance-mux --canonicalize --cse "
            "--lower-comb --canonicalize --cse --hw-legalize-modules "
            "--prettify-verilog --export-verilog " +
            shellQuote(input.string()) + " > " + shellQuote(output.string()) + " 2> " + shellQuote(error.string());
        const int raw_status = std::system(command.c_str());
        const int status = raw_status == -1 ? -1 :
#ifndef _WIN32
            (WIFEXITED(raw_status) ? WEXITSTATUS(raw_status) : 128 + WTERMSIG(raw_status));
#else
            raw_status;
#endif
        if (status != 0) {
            result.error = "circt-opt failed with exit status " + std::to_string(status) + "\n" + readFile(error);
        } else {
            const std::string exported = readFile(output);
            result.verilog = module_body ? lowerer.moduleBody(exported) : lowerer.moduleVerilog(exported);
        }
        result.input_mlir_path = input.string();
        result.optimized_mlir_path = output.string();
        result.stderr_path = error.string();
        if (!retain_intermediates && result.ok()) {
            std::error_code ec;
            std::filesystem::remove_all(directory, ec);
            result.input_mlir_path.clear(); result.optimized_mlir_path.clear(); result.stderr_path.clear();
        }
    } catch (const std::exception& error) {
        result.error = "CIRCT bridge: " + std::string(error.what());
        if (!directory.empty()) {
            result.input_mlir_path = (directory / "input.mlir").string();
            result.optimized_mlir_path = (directory / "output.txt").string();
            result.stderr_path = (directory / "circt.stderr").string();
        }
    }
    return result;
}

} // namespace pred::circt
