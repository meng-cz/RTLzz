#include "BackendIR.h"

#include <ostream>
#include <sstream>

namespace rtlzz {
namespace {

std::string opHeaderToString(const Operation& operation) {
    std::ostringstream os;
    os << operation.kind;
    if (!operation.op.empty()) os << "(" << operation.op << ")";
    if (!operation.callee.empty()) os << "(" << operation.callee << ")";
    if (operation.to_width > 0) os << "<" << operation.to_width << ">";
    if (operation.hi >= 0 || operation.lo >= 0) os << "[" << operation.hi << ":" << operation.lo << "]";
    if (operation.bit >= 0) os << "[" << operation.bit << "]";
    if (operation.times > 0) os << "x" << operation.times;
    return os.str();
}

} // namespace

std::string toString(const TypeInfo& type) {
    std::ostringstream os;
    os << (type.name.empty() ? "<unknown>" : type.name)
       << " width=" << type.width
       << " signed=" << (type.is_signed ? "true" : "false");
    if (!type.hw_kind.empty()) os << " hw_kind=" << type.hw_kind;
    if (type.is_hw_int) os << " hw_int=true";
    if (type.is_array) {
        os << " array_dims=[";
        for (std::size_t i = 0; i < type.array_dims.size(); ++i) {
            if (i) os << ",";
            os << type.array_dims[i];
        }
        os << "]";
        if (type.array_size > 0) os << " array_size=" << type.array_size;
    }
    if (!type.direction.empty()) os << " direction=" << type.direction;
    return os.str();
}

std::string toString(const Operand& operand) {
    if (operand.kind == Operand::Kind::Literal) return operand.text;
    if (operand.kind == Operand::Kind::Port) return "port." + operand.text;
    if (operand.kind == Operand::Kind::Aggregate) return "aggregate." + operand.text;
    return operand.text;
}

std::string toString(const Operation& operation) {
    std::ostringstream os;
    os << opHeaderToString(operation) << "(";
    for (std::size_t i = 0; i < operation.operands.size(); ++i) {
        if (i) os << ", ";
        os << toString(operation.operands[i]);
    }
    os << ")";
    if (operation.guard) os << " when " << toString(*operation.guard);
    os << " : " << toString(operation.type);
    return os.str();
}

void printDebug(const Program& program, std::ostream& os) {
    os << "RTLzz backend debug IR\n";
    os << "function: " << program.function_name << "\n";
    os << "metadata:\n";
    for (const auto& [key, value] : program.metadata) {
        os << "  " << key << ": " << value << "\n";
    }

    os << "\ninputs:";
    for (const auto& input : program.inputs) os << " " << input;
    os << "\noutputs:";
    for (const auto& output : program.outputs) os << " " << output;
    os << "\n";

    os << "\nports (" << program.ports.size() << ")\n";
    for (const auto& port : program.ports) {
        os << "  " << port.name << " [" << port.direction << "] : " << toString(port.type) << "\n";
        os << "    elements =";
        for (const auto& element : port.element_symbols) os << " " << element;
        os << "\n";
    }

    os << "\naggregates (" << program.aggregates.size() << ")\n";
    for (const auto& aggregate : program.aggregates) {
        os << "  " << aggregate.name << " : " << toString(aggregate.type) << "\n";
        os << "    elements =";
        for (const auto& element : aggregate.element_symbols) os << " " << element;
        os << "\n";
    }

    os << "\nsymbols (" << program.symbols.size() << ")\n";
    for (const auto& symbol : program.symbols) {
        os << "  " << symbol.name;
        if (!symbol.port_name.empty()) {
            os << " [port_element " << symbol.port_name;
            if (symbol.port_element_index >= 0) os << "[" << symbol.port_element_index << "]";
            os << "]";
        }
        os << " : " << toString(symbol.type) << "\n";
        if (symbol.driver) {
            os << "    driver = " << toString(*symbol.driver);
            if (symbol.driver->has_source_assignment) {
                os << " source#" << symbol.driver->source_assignment;
            }
            os << "\n";
        } else {
            os << "    driver = <none>\n";
        }
    }
}

} // namespace rtlzz
