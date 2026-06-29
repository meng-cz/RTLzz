#include "PredicateJsonLoader.h"

#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace rtlzz {
namespace {

class Loader {
public:
    Program load(const json::Value& root) {
        program_.metadata["schema_version"] = stringOrEmpty(root.find("schema_version"));
        program_.metadata["tool_version"] = stringOrEmpty(root.find("tool_version"));
        program_.metadata["build_commit"] = stringOrEmpty(root.find("build_commit"));
        program_.function_name = stringOrEmpty(root.find("function"));

        readStringArray(root.find("inputs"), program_.inputs);
        readStringArray(root.find("outputs"), program_.outputs);
        rejectInOutPorts(root.find("param_directions"));

        const json::Value* symbols = root.find("symbols");
        readPortAndAggregateShapes(symbols, root.find("param_directions"));
        readScalarSymbols(symbols);
        connectInputPorts();
        readAssignments(root.at("assignments"));
        readOutputExpressions(root.find("output_expressions"));
        return std::move(program_);
    }

private:
    Program program_;
    std::size_t next_temp_ = 0;
    std::unordered_map<std::string, std::string> directions_;

    static std::string stringOrEmpty(const json::Value* value) {
        return value && value->isString() ? value->asString() : "";
    }

    static bool boolOrFalse(const json::Value* value) {
        return value && value->isBool() ? value->asBool() : false;
    }

    static int intOrZero(const json::Value* value) {
        return value && value->isNumber() ? value->asInt() : 0;
    }

    static void readStringArray(const json::Value* value, std::vector<std::string>& out) {
        if (!value) return;
        for (const auto& item : value->asArray()) out.push_back(item.asString());
    }

    static std::string baseSignalName(const std::string& name) {
        std::size_t pos = name.size();
        while (pos > 0 && std::isdigit(static_cast<unsigned char>(name[pos - 1]))) --pos;
        if (pos == name.size() || pos == 0 || name[pos - 1] != '_') return name;
        return name.substr(0, pos - 1);
    }

    static bool isInitialSsaVersion(const std::string& name) {
        std::size_t pos = name.size();
        while (pos > 0 && std::isdigit(static_cast<unsigned char>(name[pos - 1]))) --pos;
        if (pos == name.size() || pos == 0 || name[pos - 1] != '_') return false;
        return name.substr(pos) == "0";
    }

    static int flattenedArraySize(const TypeInfo& type) {
        if (!type.array_dims.empty()) {
            int size = 1;
            for (int dim : type.array_dims) size *= dim;
            return size;
        }
        return type.array_size;
    }

    static TypeInfo scalarElementType(TypeInfo type) {
        type.is_array = false;
        type.array_size = 0;
        type.array_dims.clear();
        return type;
    }

    static TypeInfo readType(const json::Value& node) {
        TypeInfo type;
        type.name = stringOrEmpty(node.find("type"));
        type.width = intOrZero(node.find("width"));
        type.is_signed = boolOrFalse(node.find("signed"));
        type.is_hw_int = boolOrFalse(node.find("is_hw_int"));
        type.hw_kind = stringOrEmpty(node.find("hw_kind"));
        type.is_array = boolOrFalse(node.find("is_array"));
        type.array_size = intOrZero(node.find("array_size"));
        type.direction = stringOrEmpty(node.find("direction"));
        if (const auto* dims = node.find("array_dims")) {
            for (const auto& dim : dims->asArray()) type.array_dims.push_back(dim.asInt());
            if (!type.array_dims.empty()) type.is_array = true;
        }
        return type;
    }

    static void rejectInOutPorts(const json::Value* param_directions) {
        if (!param_directions) return;
        for (const auto& [name, direction] : param_directions->asObject()) {
            if (direction.asString() == "InOut") {
                throw std::runtime_error("InOut ports are forbidden by backend convention: " + name);
            }
        }
    }

    void readDirections(const json::Value* param_directions) {
        if (!param_directions) return;
        for (const auto& [name, direction] : param_directions->asObject()) {
            directions_[name] = direction.asString();
        }
    }

    void readPortAndAggregateShapes(const json::Value* symbols, const json::Value* param_directions) {
        readDirections(param_directions);
        if (!symbols) return;

        for (const auto& [name, value] : symbols->asObject()) {
            TypeInfo type = readType(value);
            auto direction_it = directions_.find(name);
            if (direction_it != directions_.end()) {
                ensurePort(name, direction_it->second, type);
            } else if (type.is_array) {
                ensureAggregate(name, type);
            }
        }
    }

    void readScalarSymbols(const json::Value* symbols) {
        if (!symbols) return;
        for (const auto& [name, value] : symbols->asObject()) {
            TypeInfo type = readType(value);
            if (type.is_array) continue;
            if (program_.port_index.count(name) || program_.aggregate_index.count(name)) continue;
            ensureSymbol(name, type);
        }
    }

    PortInfo& ensurePort(const std::string& name, const std::string& direction, TypeInfo type) {
        auto it = program_.port_index.find(name);
        if (it != program_.port_index.end()) return program_.ports[it->second];

        std::size_t index = program_.ports.size();
        program_.port_index.emplace(name, index);
        PortInfo port;
        port.name = name;
        port.direction = direction;
        port.type = type;

        TypeInfo element_type = scalarElementType(type);
        if (type.is_array) {
            int count = flattenedArraySize(type);
            for (int i = 0; i < count; ++i) {
                std::string element_name = name + "_" + std::to_string(i);
                port.element_symbols.push_back(element_name);
                SymbolInfo& element = ensureSymbol(element_name, element_type);
                element.port_name = name;
                element.port_element_index = i;
            }
        } else {
            port.element_symbols.push_back(name);
            SymbolInfo& element = ensureSymbol(name, element_type);
            element.port_name = name;
            element.port_element_index = 0;
        }

        program_.ports.push_back(std::move(port));
        return program_.ports.back();
    }

    AggregateInfo& ensureAggregate(const std::string& name, TypeInfo type) {
        auto it = program_.aggregate_index.find(name);
        if (it != program_.aggregate_index.end()) return program_.aggregates[it->second];

        std::size_t index = program_.aggregates.size();
        program_.aggregate_index.emplace(name, index);
        AggregateInfo aggregate;
        aggregate.name = name;
        aggregate.type = type;

        TypeInfo element_type = scalarElementType(type);
        int count = flattenedArraySize(type);
        for (int i = 0; i < count; ++i) {
            std::string element_name = name + "_" + std::to_string(i);
            aggregate.element_symbols.push_back(element_name);
            ensureSymbol(element_name, element_type);
        }

        program_.aggregates.push_back(std::move(aggregate));
        return program_.aggregates.back();
    }

    void connectInputPorts() {
        for (const auto& port : program_.ports) {
            if (port.direction != "Input") continue;
            for (std::size_t i = 0; i < port.element_symbols.size(); ++i) {
                Operation op;
                op.kind = "port_read";
                op.type = symbolType(port.element_symbols[i]);
                op.operands.push_back(portOperand(port.name, port.type));
                if (port.element_symbols.size() > 1) {
                    op.operands.push_back(literalOperand(std::to_string(i), TypeInfo{"int", 32, true}));
                }
                setDriver(port.element_symbols[i], std::move(op), true);
            }
        }
    }

    void readAssignments(const json::Value& assignments) {
        std::size_t index = 0;
        for (const auto& item : assignments.asArray()) {
            Operand guard = flattenExpr(item.at("guard_expr"));
            Operand target = flattenTarget(item.at("target_expr"));
            Operand value = flattenExpr(item.at("value_expr"));

            Operation op;
            op.kind = "assign";
            op.operands.push_back(value);
            op.type = target.type.width ? target.type : value.type;
            op.source_assignment = index;
            op.has_source_assignment = true;
            op.guard = guard;
            setDriver(target.text, std::move(op), false);
            ++index;
        }
    }

    void readOutputExpressions(const json::Value* output_expressions) {
        if (!output_expressions) return;
        for (const auto& [name, value] : output_expressions->asObject()) {
            Operand rhs = flattenExpr(value.at("expr_tree"));
            SymbolInfo& output = ensureSymbol(name, readOutputType(value, value.at("expr_tree")));

            Operation op;
            op.kind = "assign";
            op.operands.push_back(rhs);
            op.type = output.type.width ? output.type : rhs.type;
            setDriver(name, std::move(op), false);
        }
    }

    static TypeInfo readOutputType(const json::Value& output_node, const json::Value& expr) {
        TypeInfo type = readType(expr);
        if (const auto* width = output_node.find("width")) type.width = width->asInt();
        if (const auto* sign = output_node.find("signed")) type.is_signed = sign->asBool();
        if (const auto* hw_kind = output_node.find("hw_kind")) type.hw_kind = hw_kind->asString();
        return type;
    }

    Operand flattenTarget(const json::Value& expr) {
        if (expr.at("kind").asString() != "var") {
            throw std::runtime_error("assignment target must flatten to a named symbol");
        }
        return symbolOperand(expr.at("name").asString(), readType(expr), false);
    }

    Operand flattenExpr(const json::Value& expr) {
        const std::string kind = expr.at("kind").asString();
        if (kind == "literal") {
            return literalOperand(expr.at("value").asString(), readType(expr));
        }
        if (kind == "var") {
            return symbolOperand(expr.at("name").asString(), readType(expr), true);
        }

        const TypeInfo type = readType(expr);
        const std::string temp_name = makeTempName();
        ensureSymbol(temp_name, type);

        Operation op;
        op.kind = kind;
        op.type = type;

        if (kind == "binary") {
            op.op = expr.at("op").asString();
            op.operands.push_back(flattenExpr(expr.at("left")));
            op.operands.push_back(flattenExpr(expr.at("right")));
        } else if (kind == "unary") {
            op.op = expr.at("op").asString();
            op.operands.push_back(flattenExpr(expr.at("operand")));
        } else if (kind == "ite") {
            op.operands.push_back(flattenExpr(expr.at("cond")));
            op.operands.push_back(flattenExpr(expr.at("then")));
            op.operands.push_back(flattenExpr(expr.at("else")));
        } else if (kind == "array_access") {
            op.operands.push_back(flattenExpr(expr.at("base")));
            op.operands.push_back(flattenExpr(expr.at("index")));
        } else if (kind == "field_access") {
            op.op = expr.at("field").asString();
            op.operands.push_back(flattenExpr(expr.at("base")));
        } else if (kind == "call" || kind == "dynamic_slice" || kind == "dynamic_bit_select") {
            op.callee = stringOrEmpty(expr.find("callee"));
            const auto& args = expr.at("args").asArray();
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (op.callee == "lookup" && i == 0 && args[i].at("kind").asString() == "literal") {
                    std::string aggregate_name = args[i].at("value").asString();
                    if (program_.aggregate_index.count(aggregate_name)) {
                        Operand operand;
                        operand.kind = Operand::Kind::Aggregate;
                        operand.text = aggregate_name;
                        operand.type = program_.aggregates[program_.aggregate_index.at(aggregate_name)].type;
                        op.operands.push_back(std::move(operand));
                        continue;
                    }
                }
                op.operands.push_back(flattenExpr(args[i]));
            }
        } else if (kind == "cast" || kind == "zext" || kind == "sext" || kind == "trunc") {
            op.to_width = intOrZero(expr.find("to_width"));
            op.operands.push_back(flattenExpr(expr.at("expr")));
        } else if (kind == "slice") {
            op.hi = expr.at("hi").asInt();
            op.lo = expr.at("lo").asInt();
            op.operands.push_back(flattenExpr(expr.at("base")));
        } else if (kind == "bit_select") {
            op.bit = expr.at("bit").asInt();
            op.operands.push_back(flattenExpr(expr.at("base")));
        } else if (kind == "write_slice") {
            op.hi = expr.at("hi").asInt();
            op.lo = expr.at("lo").asInt();
            op.operands.push_back(flattenExpr(expr.at("base")));
            op.operands.push_back(flattenExpr(expr.at("value")));
        } else if (kind == "write_bit") {
            op.bit = expr.at("bit").asInt();
            op.operands.push_back(flattenExpr(expr.at("base")));
            op.operands.push_back(flattenExpr(expr.at("value")));
        } else if (kind == "concat") {
            for (const auto& part : expr.at("parts").asArray()) {
                op.operands.push_back(flattenExpr(part));
            }
        } else if (kind == "repeat") {
            op.times = expr.at("times").asInt();
            op.operands.push_back(flattenExpr(expr.at("expr")));
        } else if (kind == "reduce_or" || kind == "reduce_and" || kind == "reduce_xor") {
            op.operands.push_back(flattenExpr(expr.at("expr")));
        } else {
            throw std::runtime_error("unsupported expression kind in predicate JSON: " + kind);
        }

        setDriver(temp_name, std::move(op), false);
        return symbolOperand(temp_name, type, true);
    }

    Operand symbolOperand(const std::string& name, TypeInfo type, bool read_context) {
        if (type.is_array || program_.aggregate_index.count(name)) {
            ensureAggregate(name, type);
            Operand operand;
            operand.kind = Operand::Kind::Aggregate;
            operand.text = name;
            operand.type = type;
            return operand;
        }

        if (read_context) rejectForbiddenOutputRead(name);

        SymbolInfo& symbol = ensureSymbol(name, type);
        connectInitialSsaVersion(symbol);

        Operand operand;
        operand.kind = Operand::Kind::Symbol;
        operand.text = symbol.name;
        operand.type = symbol.type;
        return operand;
    }

    Operand literalOperand(std::string value, TypeInfo type) {
        Operand operand;
        operand.kind = Operand::Kind::Literal;
        operand.text = std::move(value);
        operand.type = std::move(type);
        return operand;
    }

    Operand portOperand(std::string name, TypeInfo type) {
        Operand operand;
        operand.kind = Operand::Kind::Port;
        operand.text = std::move(name);
        operand.type = std::move(type);
        return operand;
    }

    SymbolInfo& ensureSymbol(const std::string& name, TypeInfo type) {
        if (type.is_array) {
            throw std::runtime_error("array value cannot be inserted into scalar symbol table: " + name);
        }

        auto it = program_.symbol_index.find(name);
        if (it == program_.symbol_index.end()) {
            std::size_t index = program_.symbols.size();
            program_.symbol_index.emplace(name, index);
            SymbolInfo symbol;
            symbol.name = name;
            symbol.type = std::move(type);
            program_.symbols.push_back(std::move(symbol));
            return program_.symbols.back();
        }

        SymbolInfo& symbol = program_.symbols[it->second];
        mergeType(symbol.type, type);
        return symbol;
    }

    TypeInfo symbolType(const std::string& name) {
        auto it = program_.symbol_index.find(name);
        if (it == program_.symbol_index.end()) return {};
        return program_.symbols[it->second].type;
    }

    void mergeType(TypeInfo& existing, const TypeInfo& incoming) {
        if (existing.name.empty()) existing.name = incoming.name;
        if (existing.width == 0) existing.width = incoming.width;
        if (!existing.is_signed) existing.is_signed = incoming.is_signed;
        if (!existing.is_hw_int) existing.is_hw_int = incoming.is_hw_int;
        if (existing.hw_kind.empty()) existing.hw_kind = incoming.hw_kind;
        if (existing.direction.empty()) existing.direction = incoming.direction;
    }

    void connectInitialSsaVersion(SymbolInfo& symbol) {
        if (symbol.driver || !isInitialSsaVersion(symbol.name)) return;
        const std::string base = baseSignalName(symbol.name);
        auto base_it = program_.symbol_index.find(base);
        if (base_it == program_.symbol_index.end()) return;

        const SymbolInfo& base_symbol = program_.symbols[base_it->second];
        if (base_symbol.port_name.empty()) return;
        PortInfo& port = program_.ports[program_.port_index.at(base_symbol.port_name)];
        if (port.direction == "Output") {
            return;
        }

        Operation op;
        op.kind = "assign";
        op.type = symbol.type.width ? symbol.type : base_symbol.type;
        Operand operand;
        operand.kind = Operand::Kind::Symbol;
        operand.text = base_symbol.name;
        operand.type = base_symbol.type;
        op.operands.push_back(operand);
        setDriver(symbol.name, std::move(op), true);
    }

    void rejectForbiddenOutputRead(const std::string& name) {
        std::string base = isInitialSsaVersion(name) ? baseSignalName(name) : name;
        auto it = program_.symbol_index.find(base);
        if (it == program_.symbol_index.end()) return;
        const SymbolInfo& symbol = program_.symbols[it->second];
        if (symbol.port_name.empty()) return;
        const PortInfo& port = program_.ports[program_.port_index.at(symbol.port_name)];
        if (port.direction == "Output") {
            throw std::runtime_error("Output ports may not be read by backend convention: " + name);
        }
    }

    void setDriver(const std::string& symbol_name, Operation op, bool allow_existing_same_kind) {
        SymbolInfo& symbol = ensureSymbol(symbol_name, op.type);
        if (symbol.driver) {
            if (allow_existing_same_kind && symbol.driver->kind == op.kind) return;
            throw std::runtime_error("symbol has more than one driver operation: " + symbol_name);
        }
        symbol.driver = std::move(op);
    }

    std::string makeTempName() {
        while (true) {
            std::string name = "__rtlzz_t" + std::to_string(next_temp_++);
            if (!program_.symbol_index.count(name) && !program_.aggregate_index.count(name)) return name;
        }
    }
};

} // namespace

Program loadPredicateJson(const json::Value& root) {
    return Loader().load(root);
}

} // namespace rtlzz
