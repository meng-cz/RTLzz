#include "emitter/ListJsonEmitter.h"

#include <algorithm>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace pred {
namespace {

constexpr const char* kSchemaVersion = "rtlzz-signal-list-json-v1";
constexpr const char* kToolVersion = "predicate-expand-0.1";

#ifdef PREDICATE_EXPAND_BUILD_COMMIT
constexpr const char* kBuildCommit = PREDICATE_EXPAND_BUILD_COMMIT;
#else
constexpr const char* kBuildCommit = "unknown";
#endif

struct Operand {
    std::string kind = "symbol";
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

struct Signal {
    std::string name;
    TypeInfo type;
    std::string port_name;
    int port_element_index = -1;
    std::optional<Operation> driver;
};

struct Port {
    std::string name;
    std::string direction;
    TypeInfo type;
    std::vector<std::string> element_symbols;
};

struct Aggregate {
    std::string name;
    TypeInfo type;
    std::vector<std::string> element_symbols;
};

struct ListProgram {
    std::string function_name;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::vector<Port> ports;
    std::unordered_map<std::string, std::size_t> port_index;
    std::vector<Aggregate> aggregates;
    std::unordered_map<std::string, std::size_t> aggregate_index;
    std::vector<Signal> signals;
    std::unordered_map<std::string, std::size_t> signal_index;
};

static std::string ind(int level) {
    return std::string(level * 2, ' ');
}

static std::string jsonEscape(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c; break;
        }
    }
    return result;
}

template <typename Map>
static std::vector<std::string> sortedKeys(const Map& map) {
    std::vector<std::string> keys;
    keys.reserve(map.size());
    for (const auto& [key, value] : map) {
        (void)value;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
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

static std::string exprKindName(const ExprPtr& e) {
    switch (e->kind) {
    case ExprKind::Literal: return "literal";
    case ExprKind::VarRef: return "var";
    case ExprKind::BinaryOp: return "binary";
    case ExprKind::UnaryOp: return "unary";
    case ExprKind::ArrayAccess: return "array_access";
    case ExprKind::FieldAccess: return "field_access";
    case ExprKind::Call:
        if (e->intrinsic == IntrinsicKind::DynamicBitAt) return "dynamic_bit_select";
        if (e->intrinsic == IntrinsicKind::DynamicRangeAt) return "dynamic_slice";
        return "call";
    case ExprKind::Cast: return "cast";
    case ExprKind::Ternary: return "ite";
    case ExprKind::ZExt: return "zext";
    case ExprKind::SExt: return "sext";
    case ExprKind::Trunc: return "trunc";
    case ExprKind::Slice: return "slice";
    case ExprKind::BitSelect: return "bit_select";
    case ExprKind::WriteSlice: return "write_slice";
    case ExprKind::WriteBit: return "write_bit";
    case ExprKind::Concat: return "concat";
    case ExprKind::Repeat: return "repeat";
    case ExprKind::ReduceOr: return "reduce_or";
    case ExprKind::ReduceAnd: return "reduce_and";
    case ExprKind::ReduceXor: return "reduce_xor";
    }
    return "unknown";
}

class Builder {
public:
    ListProgram build(const PredicateProgram& source) {
        program_.function_name = source.function_name;
        program_.outputs = source.outputs;
        if (program_.outputs.empty()) {
            for (const auto& [name, direction] : source.param_directions) {
                if (direction == "Output") program_.outputs.push_back(name);
            }
            std::sort(program_.outputs.begin(), program_.outputs.end());
        }

        for (const auto& name : sortedKeys(source.symbols)) {
            const TypeInfo& type = source.symbols.at(name);
            auto dir = source.param_directions.find(name);
            if (dir != source.param_directions.end()) {
                ensurePort(name, dir->second, type);
                if (dir->second == "Input") program_.inputs.push_back(name);
            } else if (type.is_array) {
                ensureAggregate(name, type);
            } else {
                ensureSignal(name, type);
            }
        }
        std::sort(program_.inputs.begin(), program_.inputs.end());

        connectInputPorts();
        for (std::size_t i = 0; i < source.assignments.size(); ++i) {
            const auto& assign = source.assignments[i];
            Operand guard = flattenExpr(assign.guard ? assign.guard : make_true_guard());
            Operand target = flattenTarget(assign.target);
            Operand value = flattenExpr(assign.value);

            Operation op;
            op.kind = "assign";
            op.operands.push_back(std::move(value));
            op.type = target.type.width ? target.type : assign.type;
            op.source_assignment = i;
            op.has_source_assignment = true;
            op.guard = std::move(guard);
            setDriver(target.text, std::move(op), false);
        }

        for (const auto& output : source.output_expressions) {
            Operand rhs = flattenExpr(output.expr);
            Signal& signal = ensureSignal(output.name, output.type);
            Operation op;
            op.kind = "assign";
            op.operands.push_back(std::move(rhs));
            op.type = signal.type.width ? signal.type : output.type;
            setDriver(output.name, std::move(op), false);
        }

        return std::move(program_);
    }

private:
    ListProgram program_;
    std::size_t next_temp_ = 0;

    Signal& ensureSignal(const std::string& name, TypeInfo type) {
        if (type.is_array) {
            throw std::runtime_error("listjson scalar signal cannot use array type: " + name);
        }
        auto it = program_.signal_index.find(name);
        if (it == program_.signal_index.end()) {
            std::size_t index = program_.signals.size();
            program_.signal_index.emplace(name, index);
            Signal signal;
            signal.name = name;
            signal.type = std::move(type);
            program_.signals.push_back(std::move(signal));
            return program_.signals.back();
        }
        Signal& signal = program_.signals[it->second];
        mergeType(signal.type, type);
        return signal;
    }

    Port& ensurePort(const std::string& name, const std::string& direction, TypeInfo type) {
        auto it = program_.port_index.find(name);
        if (it != program_.port_index.end()) return program_.ports[it->second];

        std::size_t index = program_.ports.size();
        program_.port_index.emplace(name, index);
        Port port;
        port.name = name;
        port.direction = direction;
        port.type = type;

        TypeInfo element_type = scalarElementType(type);
        if (type.is_array) {
            int count = flattenedArraySize(type);
            for (int i = 0; i < count; ++i) {
                std::string element_name = name + "_" + std::to_string(i);
                port.element_symbols.push_back(element_name);
                Signal& element = ensureSignal(element_name, element_type);
                element.port_name = name;
                element.port_element_index = i;
            }
        } else {
            port.element_symbols.push_back(name);
            Signal& element = ensureSignal(name, element_type);
            element.port_name = name;
            element.port_element_index = 0;
        }

        program_.ports.push_back(std::move(port));
        return program_.ports.back();
    }

    Aggregate& ensureAggregate(const std::string& name, TypeInfo type) {
        auto it = program_.aggregate_index.find(name);
        if (it != program_.aggregate_index.end()) return program_.aggregates[it->second];

        std::size_t index = program_.aggregates.size();
        program_.aggregate_index.emplace(name, index);
        Aggregate aggregate;
        aggregate.name = name;
        aggregate.type = type;

        TypeInfo element_type = scalarElementType(type);
        int count = flattenedArraySize(type);
        for (int i = 0; i < count; ++i) {
            std::string element_name = name + "_" + std::to_string(i);
            aggregate.element_symbols.push_back(element_name);
            ensureSignal(element_name, element_type);
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
                op.type = signalType(port.element_symbols[i]);
                op.operands.push_back(portOperand(port.name, port.type));
                if (port.element_symbols.size() > 1) {
                    op.operands.push_back(literalOperand(std::to_string(i), TypeInfo{"int", 32, true}));
                }
                setDriver(port.element_symbols[i], std::move(op), true);
            }
        }
    }

    Operand flattenTarget(const ExprPtr& expr) {
        if (!expr) throw std::runtime_error("listjson assignment target is null");
        if (expr->kind == ExprKind::VarRef) {
            return symbolOperand(expr->var_name, expr->type);
        }
        if (expr->kind == ExprKind::ArrayAccess && expr->array_base &&
            expr->index && expr->index->kind == ExprKind::Literal) {
            Operand base = flattenTarget(expr->array_base);
            return symbolOperand(base.text + "_" + expr->index->literal_value,
                                 scalarElementType(expr->type));
        }
        throw std::runtime_error("listjson assignment target must flatten to a named signal");
    }

    Operand flattenExpr(const ExprPtr& expr) {
        if (!expr) return literalOperand("1", TypeInfo{"bool", 1, false});
        if (expr->kind == ExprKind::Literal) {
            return literalOperand(expr->literal_value, expr->type);
        }
        if (expr->kind == ExprKind::VarRef) {
            return symbolOperand(expr->var_name, expr->type);
        }
        if (expr->kind == ExprKind::ArrayAccess && expr->array_base &&
            expr->index && expr->index->kind == ExprKind::Literal) {
            Operand base = flattenExpr(expr->array_base);
            return symbolOperand(base.text + "_" + expr->index->literal_value,
                                 scalarElementType(expr->type));
        }

        std::string temp_name = makeTempName();
        ensureSignal(temp_name, expr->type);

        Operation op;
        op.kind = exprKindName(expr);
        op.type = expr->type;

        switch (expr->kind) {
        case ExprKind::BinaryOp:
            op.op = expr->op;
            op.operands.push_back(flattenExpr(expr->left));
            op.operands.push_back(flattenExpr(expr->right));
            break;
        case ExprKind::UnaryOp:
            op.op = expr->op;
            op.operands.push_back(flattenExpr(expr->operand));
            break;
        case ExprKind::ArrayAccess:
            op.operands.push_back(flattenExpr(expr->array_base));
            op.operands.push_back(flattenExpr(expr->index));
            break;
        case ExprKind::FieldAccess:
            op.op = expr->field_name;
            op.operands.push_back(flattenExpr(expr->struct_base));
            break;
        case ExprKind::Call:
            op.callee = expr->callee;
            for (const auto& arg : expr->args) op.operands.push_back(flattenExpr(arg));
            break;
        case ExprKind::Cast:
            op.to_width = expr->to_width;
            if (op.to_width == 0) op.to_width = expr->cast_type.width;
            op.operands.push_back(flattenExpr(expr->cast_expr));
            break;
        case ExprKind::Ternary:
            op.operands.push_back(flattenExpr(expr->cond));
            op.operands.push_back(flattenExpr(expr->then_expr));
            op.operands.push_back(flattenExpr(expr->else_expr));
            break;
        case ExprKind::ZExt:
        case ExprKind::SExt:
        case ExprKind::Trunc:
            op.to_width = expr->to_width;
            op.operands.push_back(flattenExpr(expr->cast_expr));
            break;
        case ExprKind::Slice:
            op.hi = expr->hi;
            op.lo = expr->lo;
            op.operands.push_back(flattenExpr(expr->base));
            break;
        case ExprKind::BitSelect:
            op.bit = expr->bit;
            op.operands.push_back(flattenExpr(expr->base));
            break;
        case ExprKind::WriteSlice:
            op.hi = expr->hi;
            op.lo = expr->lo;
            op.operands.push_back(flattenExpr(expr->base));
            op.operands.push_back(flattenExpr(expr->value));
            break;
        case ExprKind::WriteBit:
            op.bit = expr->bit;
            op.operands.push_back(flattenExpr(expr->base));
            op.operands.push_back(flattenExpr(expr->value));
            break;
        case ExprKind::Concat:
            for (const auto& part : expr->parts) op.operands.push_back(flattenExpr(part));
            break;
        case ExprKind::Repeat:
            op.times = expr->times;
            op.operands.push_back(flattenExpr(expr->operand));
            break;
        case ExprKind::ReduceOr:
        case ExprKind::ReduceAnd:
        case ExprKind::ReduceXor:
            op.operands.push_back(flattenExpr(expr->operand));
            break;
        case ExprKind::Literal:
        case ExprKind::VarRef:
            break;
        }

        setDriver(temp_name, std::move(op), false);
        return symbolOperand(temp_name, expr->type);
    }

    Operand symbolOperand(const std::string& name, TypeInfo type) {
        if (type.is_array || program_.aggregate_index.count(name)) {
            ensureAggregate(name, type);
            Operand operand;
            operand.kind = "aggregate";
            operand.text = name;
            operand.type = std::move(type);
            return operand;
        }

        Signal& signal = ensureSignal(name, type);
        Operand operand;
        operand.kind = "symbol";
        operand.text = signal.name;
        operand.type = signal.type;
        return operand;
    }

    Operand literalOperand(std::string value, TypeInfo type) {
        Operand operand;
        operand.kind = "literal";
        operand.text = std::move(value);
        operand.type = std::move(type);
        return operand;
    }

    Operand portOperand(std::string value, TypeInfo type) {
        Operand operand;
        operand.kind = "port";
        operand.text = std::move(value);
        operand.type = std::move(type);
        return operand;
    }

    TypeInfo signalType(const std::string& name) const {
        auto it = program_.signal_index.find(name);
        if (it == program_.signal_index.end()) return {};
        return program_.signals[it->second].type;
    }

    void setDriver(const std::string& signal_name, Operation op, bool allow_existing_same_kind) {
        Signal& signal = ensureSignal(signal_name, op.type);
        if (signal.driver) {
            if (allow_existing_same_kind && signal.driver->kind == op.kind) return;
            throw std::runtime_error("listjson signal has more than one driver: " + signal_name);
        }
        signal.driver = std::move(op);
    }

    void mergeType(TypeInfo& existing, const TypeInfo& incoming) {
        if (existing.name.empty()) existing.name = incoming.name;
        if (existing.width == 0) existing.width = incoming.width;
        if (!existing.is_signed) existing.is_signed = incoming.is_signed;
        if (!existing.is_hw_int) existing.is_hw_int = incoming.is_hw_int;
        if (existing.hw_kind.empty()) existing.hw_kind = incoming.hw_kind;
        if (existing.struct_name.empty()) existing.struct_name = incoming.struct_name;
    }

    std::string makeTempName() {
        while (true) {
            std::string name = "__rtlzz_t" + std::to_string(next_temp_++);
            if (!program_.signal_index.count(name) && !program_.aggregate_index.count(name)) return name;
        }
    }
};

static void emitType(std::ostream& os, const TypeInfo& type) {
    os << "{"
       << "\"name\": \"" << jsonEscape(type.name) << "\", "
       << "\"width\": " << type.width << ", "
       << "\"signed\": " << (type.is_signed ? "true" : "false") << ", "
       << "\"is_hw_int\": " << (type.is_hw_int ? "true" : "false") << ", "
       << "\"hw_kind\": \"" << jsonEscape(type.hw_kind) << "\", "
       << "\"is_array\": " << (type.is_array ? "true" : "false") << ", "
       << "\"array_size\": " << type.array_size << ", "
       << "\"array_dims\": [";
    for (std::size_t i = 0; i < type.array_dims.size(); ++i) {
        if (i) os << ", ";
        os << type.array_dims[i];
    }
    os << "]";
    if (!type.struct_name.empty()) {
        os << ", \"struct_name\": \"" << jsonEscape(type.struct_name) << "\"";
    }
    os << "}";
}

static void emitOperand(std::ostream& os, const Operand& operand, int indent) {
    os << "{\n";
    os << ind(indent + 1) << "\"kind\": \"" << jsonEscape(operand.kind) << "\",\n";
    os << ind(indent + 1) << "\"text\": \"" << jsonEscape(operand.text) << "\",\n";
    os << ind(indent + 1) << "\"type\": ";
    emitType(os, operand.type);
    os << "\n" << ind(indent) << "}";
}

static void emitOperation(std::ostream& os, const Operation& op, int indent) {
    os << "{\n";
    os << ind(indent + 1) << "\"kind\": \"" << jsonEscape(op.kind) << "\",\n";
    os << ind(indent + 1) << "\"op\": \"" << jsonEscape(op.op) << "\",\n";
    os << ind(indent + 1) << "\"callee\": \"" << jsonEscape(op.callee) << "\",\n";
    os << ind(indent + 1) << "\"type\": ";
    emitType(os, op.type);
    os << ",\n";
    os << ind(indent + 1) << "\"to_width\": " << op.to_width << ",\n";
    os << ind(indent + 1) << "\"hi\": " << op.hi << ",\n";
    os << ind(indent + 1) << "\"lo\": " << op.lo << ",\n";
    os << ind(indent + 1) << "\"bit\": " << op.bit << ",\n";
    os << ind(indent + 1) << "\"times\": " << op.times << ",\n";
    os << ind(indent + 1) << "\"source_assignment\": ";
    if (op.has_source_assignment) os << op.source_assignment;
    else os << "null";
    os << ",\n";
    os << ind(indent + 1) << "\"guard\": ";
    if (op.guard) emitOperand(os, *op.guard, indent + 1);
    else os << "null";
    os << ",\n";
    os << ind(indent + 1) << "\"operands\": [";
    if (!op.operands.empty()) os << "\n";
    for (std::size_t i = 0; i < op.operands.size(); ++i) {
        os << ind(indent + 2);
        emitOperand(os, op.operands[i], indent + 2);
        if (i + 1 < op.operands.size()) os << ",";
        os << "\n";
    }
    os << ind(indent + 1) << "]\n";
    os << ind(indent) << "}";
}

} // namespace

std::string emitListJson(const PredicateProgram& prog) {
    ListProgram list = Builder().build(prog);
    std::ostringstream os;

    os << "{\n";
    os << "  \"schema_version\": \"" << jsonEscape(kSchemaVersion) << "\",\n";
    os << "  \"tool_version\": \"" << jsonEscape(kToolVersion) << "\",\n";
    os << "  \"build_commit\": \"" << jsonEscape(kBuildCommit) << "\",\n";
    os << "  \"function\": \"" << jsonEscape(list.function_name) << "\",\n";

    os << "  \"inputs\": [";
    for (std::size_t i = 0; i < list.inputs.size(); ++i) {
        if (i) os << ", ";
        os << "\"" << jsonEscape(list.inputs[i]) << "\"";
    }
    os << "],\n";

    os << "  \"outputs\": [";
    for (std::size_t i = 0; i < list.outputs.size(); ++i) {
        if (i) os << ", ";
        os << "\"" << jsonEscape(list.outputs[i]) << "\"";
    }
    os << "],\n";

    os << "  \"ports\": [\n";
    for (std::size_t i = 0; i < list.ports.size(); ++i) {
        const auto& port = list.ports[i];
        os << "    {\n";
        os << "      \"name\": \"" << jsonEscape(port.name) << "\",\n";
        os << "      \"direction\": \"" << jsonEscape(port.direction) << "\",\n";
        os << "      \"type\": ";
        emitType(os, port.type);
        os << ",\n";
        os << "      \"element_symbols\": [";
        for (std::size_t j = 0; j < port.element_symbols.size(); ++j) {
            if (j) os << ", ";
            os << "\"" << jsonEscape(port.element_symbols[j]) << "\"";
        }
        os << "]\n";
        os << "    }";
        if (i + 1 < list.ports.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    os << "  \"aggregates\": [\n";
    for (std::size_t i = 0; i < list.aggregates.size(); ++i) {
        const auto& aggregate = list.aggregates[i];
        os << "    {\n";
        os << "      \"name\": \"" << jsonEscape(aggregate.name) << "\",\n";
        os << "      \"type\": ";
        emitType(os, aggregate.type);
        os << ",\n";
        os << "      \"element_symbols\": [";
        for (std::size_t j = 0; j < aggregate.element_symbols.size(); ++j) {
            if (j) os << ", ";
            os << "\"" << jsonEscape(aggregate.element_symbols[j]) << "\"";
        }
        os << "]\n";
        os << "    }";
        if (i + 1 < list.aggregates.size()) os << ",";
        os << "\n";
    }
    os << "  ],\n";

    os << "  \"signals\": [\n";
    for (std::size_t i = 0; i < list.signals.size(); ++i) {
        const auto& signal = list.signals[i];
        os << "    {\n";
        os << "      \"name\": \"" << jsonEscape(signal.name) << "\",\n";
        os << "      \"type\": ";
        emitType(os, signal.type);
        os << ",\n";
        os << "      \"port_name\": \"" << jsonEscape(signal.port_name) << "\",\n";
        os << "      \"port_element_index\": " << signal.port_element_index << ",\n";
        os << "      \"driver\": ";
        if (signal.driver) emitOperation(os, *signal.driver, 3);
        else os << "null";
        os << "\n";
        os << "    }";
        if (i + 1 < list.signals.size()) os << ",";
        os << "\n";
    }
    os << "  ]\n";
    os << "}\n";

    return os.str();
}

} // namespace pred
