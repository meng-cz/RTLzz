#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtlzz {

struct TypeInfo {
    std::string name;
    int width = 0;
    bool is_signed = false;
    bool is_hw_int = false;
    std::string hw_kind;
    bool is_array = false;
    int array_size = 0;
    std::vector<int> array_dims;
    std::string direction;
};

struct Operand {
    enum class Kind {
        Symbol,
        Literal,
        Port,
        Aggregate,
    };

    Kind kind = Kind::Symbol;
    std::string text;
    TypeInfo type;
};

struct Operation {
    std::string kind;
    std::string op;
    std::string callee;
    std::vector<Operand> operands;
    TypeInfo type;
    int to_width = 0;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    std::size_t source_assignment = 0;
    bool has_source_assignment = false;
    std::optional<Operand> guard;
};

struct SymbolInfo {
    std::string name;
    TypeInfo type;
    std::string port_name;
    int port_element_index = -1;
    std::optional<Operation> driver;
};

struct PortInfo {
    std::string name;
    std::string direction;
    TypeInfo type;
    std::vector<std::string> element_symbols;
};

struct AggregateInfo {
    std::string name;
    TypeInfo type;
    std::vector<std::string> element_symbols;
};

struct Program {
    std::unordered_map<std::string, std::string> metadata;
    std::string function_name;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::vector<PortInfo> ports;
    std::unordered_map<std::string, std::size_t> port_index;
    std::vector<AggregateInfo> aggregates;
    std::unordered_map<std::string, std::size_t> aggregate_index;
    std::vector<SymbolInfo> symbols;
    std::unordered_map<std::string, std::size_t> symbol_index;
};

std::string toString(const TypeInfo& type);
std::string toString(const Operand& operand);
std::string toString(const Operation& operation);
void printDebug(const Program& program, std::ostream& os);

} // namespace rtlzz
