#include "backend/beir.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace pred::beir {
namespace {

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
    type.struct_name.clear();
    return type;
}

static bool hasStructType(const TypeInfo& type) {
    return !type.struct_name.empty();
}

static TypeInfo beirType(TypeInfo type) {
    if (hasStructType(type)) {
        throw std::runtime_error("beir requires flattened scalar data, got struct type: " +
                                 type.struct_name);
    }
    return type;
}

static bool isTrueGuard(const ExprPtr& expr) {
    return !expr || (expr->kind == ExprKind::Literal &&
        (expr->literal_value == "1" || expr->literal_value == "true"));
}

static ExprPtr defaultValueFor(const TypeInfo& type) {
    TypeInfo out_type = type;
    if (out_type.width == 1 && (out_type.name == "bool" || out_type.hw_kind == "bool")) {
        return make_literal("false", out_type);
    }
    return make_literal("0", out_type);
}

static const char* operandKindText(OperandKind kind) {
    switch (kind) {
    case OperandKind::Symbol: return "symbol";
    case OperandKind::Literal: return "literal";
    case OperandKind::Port: return "port";
    case OperandKind::Aggregate: return "aggregate";
    }
    return "unknown";
}

static const char* operationKindText(OperationKind kind) {
    switch (kind) {
    case OperationKind::Assign: return "assign";
    case OperationKind::PortRead: return "port_read";
    case OperationKind::Binary: return "binary";
    case OperationKind::Unary: return "unary";
    case OperationKind::ArrayAccess: return "array_access";
    case OperationKind::Call: return "call";
    case OperationKind::Cast: return "cast";
    case OperationKind::Ite: return "ite";
    case OperationKind::ZExt: return "zext";
    case OperationKind::SExt: return "sext";
    case OperationKind::Trunc: return "trunc";
    case OperationKind::Slice: return "slice";
    case OperationKind::BitSelect: return "bit_select";
    case OperationKind::WriteSlice: return "write_slice";
    case OperationKind::WriteBit: return "write_bit";
    case OperationKind::Concat: return "concat";
    case OperationKind::Repeat: return "repeat";
    case OperationKind::ReduceOr: return "reduce_or";
    case OperationKind::ReduceAnd: return "reduce_and";
    case OperationKind::ReduceXor: return "reduce_xor";
    case OperationKind::DynamicBitSelect: return "dynamic_bit_select";
    case OperationKind::DynamicSlice: return "dynamic_slice";
    case OperationKind::DynamicWriteSlice: return "dynamic_write_slice";
    case OperationKind::DynamicWriteBit: return "dynamic_write_bit";
    case OperationKind::Lookup: return "lookup";
    }
    return "unknown";
}

static const char* opCodeText(OpCode op) {
    switch (op) {
    case OpCode::None: return "";
    case OpCode::Add: return "+";
    case OpCode::Sub: return "-";
    case OpCode::Mul: return "*";
    case OpCode::Div: return "/";
    case OpCode::Mod: return "%";
    case OpCode::BitAnd: return "&";
    case OpCode::BitOr: return "|";
    case OpCode::BitXor: return "^";
    case OpCode::LogicAnd: return "&&";
    case OpCode::LogicOr: return "||";
    case OpCode::Eq: return "==";
    case OpCode::Ne: return "!=";
    case OpCode::Lt: return "<";
    case OpCode::Le: return "<=";
    case OpCode::Gt: return ">";
    case OpCode::Ge: return ">=";
    case OpCode::Shl: return "<<";
    case OpCode::Shr: return ">>";
    case OpCode::LogicNot: return "!";
    case OpCode::BitNot: return "~";
    case OpCode::Neg: return "-";
    }
    return "";
}

static OpCode parseBinaryOpCode(const std::string& op) {
    if (op == "+") return OpCode::Add;
    if (op == "-") return OpCode::Sub;
    if (op == "*") return OpCode::Mul;
    if (op == "/") return OpCode::Div;
    if (op == "%") return OpCode::Mod;
    if (op == "&") return OpCode::BitAnd;
    if (op == "|") return OpCode::BitOr;
    if (op == "^") return OpCode::BitXor;
    if (op == "&&") return OpCode::LogicAnd;
    if (op == "||") return OpCode::LogicOr;
    if (op == "==") return OpCode::Eq;
    if (op == "!=") return OpCode::Ne;
    if (op == "<") return OpCode::Lt;
    if (op == "<=") return OpCode::Le;
    if (op == ">") return OpCode::Gt;
    if (op == ">=") return OpCode::Ge;
    if (op == "<<") return OpCode::Shl;
    if (op == ">>") return OpCode::Shr;
    throw std::runtime_error("beir unsupported binary op: " + op);
}

static OpCode parseUnaryOpCode(const std::string& op) {
    if (op == "!") return OpCode::LogicNot;
    if (op == "~") return OpCode::BitNot;
    if (op == "-") return OpCode::Neg;
    throw std::runtime_error("beir unsupported unary op: " + op);
}

static PortDirection parsePortDirection(const std::string& text) {
    if (text == "Input") return PortDirection::Input;
    if (text == "Output") return PortDirection::Output;
    if (text == "InOut") return PortDirection::InOut;
    return PortDirection::Unknown;
}

static const char* portDirectionText(PortDirection direction) {
    switch (direction) {
    case PortDirection::Input: return "Input";
    case PortDirection::Output: return "Output";
    case PortDirection::InOut: return "InOut";
    case PortDirection::Unknown: return "Unknown";
    }
    return "Unknown";
}

static OperationKind operationKindForExpr(const ExprPtr& e) {
    switch (e->kind) {
    case ExprKind::BinaryOp: return OperationKind::Binary;
    case ExprKind::UnaryOp: return OperationKind::Unary;
    case ExprKind::ArrayAccess: return OperationKind::ArrayAccess;
    case ExprKind::FieldAccess:
        throw std::runtime_error("beir rejects unflattened field access");
    case ExprKind::Call:
        if (e->intrinsic == IntrinsicKind::DynamicBitAt) return OperationKind::DynamicBitSelect;
        if (e->intrinsic == IntrinsicKind::DynamicRangeAt) return OperationKind::DynamicSlice;
        if (e->callee == "lookup") return OperationKind::Lookup;
        return OperationKind::Call;
    case ExprKind::Cast: return OperationKind::Cast;
    case ExprKind::Ternary: return OperationKind::Ite;
    case ExprKind::ZExt: return OperationKind::ZExt;
    case ExprKind::SExt: return OperationKind::SExt;
    case ExprKind::Trunc: return OperationKind::Trunc;
    case ExprKind::Slice: return OperationKind::Slice;
    case ExprKind::BitSelect: return OperationKind::BitSelect;
    case ExprKind::WriteSlice: return OperationKind::WriteSlice;
    case ExprKind::WriteBit: return OperationKind::WriteBit;
    case ExprKind::DynamicWriteSlice: return OperationKind::DynamicWriteSlice;
    case ExprKind::DynamicWriteBit: return OperationKind::DynamicWriteBit;
    case ExprKind::Concat: return OperationKind::Concat;
    case ExprKind::Repeat: return OperationKind::Repeat;
    case ExprKind::ReduceOr: return OperationKind::ReduceOr;
    case ExprKind::ReduceAnd: return OperationKind::ReduceAnd;
    case ExprKind::ReduceXor: return OperationKind::ReduceXor;
    case ExprKind::Literal:
    case ExprKind::VarRef:
        break;
    }
    throw std::runtime_error("beir expression kind does not map to an operation");
}

static std::string readableOpName(const std::string& op) {
    if (op == "+") return "add";
    if (op == "-") return "sub";
    if (op == "*") return "mul";
    if (op == "/") return "div";
    if (op == "%") return "mod";
    if (op == "&") return "and";
    if (op == "|") return "or";
    if (op == "^") return "xor";
    if (op == "&&") return "logic_and";
    if (op == "||") return "logic_or";
    if (op == "==") return "eq";
    if (op == "!=") return "ne";
    if (op == "<") return "lt";
    if (op == "<=") return "le";
    if (op == ">") return "gt";
    if (op == ">=") return "ge";
    if (op == "<<") return "shl";
    if (op == ">>") return "shr";
    if (op == "!") return "not";
    if (op == "~") return "bit_not";
    return op.empty() ? "op" : op;
}

static std::string sanitizeNamePart(const std::string& text) {
    std::string out;
    for (char ch : text) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
        else if (ch == '_') out.push_back('_');
        else if (out.empty() || out.back() != '_') out.push_back('_');
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "tmp" : out;
}

static std::string tempStemForExpr(const ExprPtr& expr) {
    if (!expr) return "const";
    switch (expr->kind) {
    case ExprKind::BinaryOp:
        return readableOpName(expr->op);
    case ExprKind::UnaryOp:
        return readableOpName(expr->op);
    case ExprKind::Ternary:
        return "mux";
    case ExprKind::Call:
        if (expr->callee == "lookup") return "lookup";
        if (!expr->callee.empty()) return expr->callee;
        return operationKindText(operationKindForExpr(expr));
    case ExprKind::FieldAccess:
        throw std::runtime_error("beir rejects unflattened field access: " + expr->field_name);
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc:
        return std::string(operationKindText(operationKindForExpr(expr))) + "_" +
               std::to_string(expr->to_width);
    case ExprKind::Slice:
        return "slice_" + std::to_string(expr->hi) + "_" + std::to_string(expr->lo);
    case ExprKind::BitSelect:
        return "bit_" + std::to_string(expr->bit);
    case ExprKind::Repeat:
        return "repeat_" + std::to_string(expr->times);
    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor:
        return operationKindText(operationKindForExpr(expr));
    default:
        return operationKindText(operationKindForExpr(expr));
    }
}

struct PendingAssignment {
    ExprPtr guard;
    ExprPtr value;
    TypeInfo type;
    DebugLoc debug_loc;
};

class Builder {
public:
    Program build(const PredicateProgram& source) {
        program_.function_name = source.function_name;
        program_.outputs = source.outputs;
        for (const auto& name : sortedKeys(source.lookup_tables)) {
            LookupTable table;
            table.name = name;
            table.values = source.lookup_tables.at(name);
            program_.lookup_tables.push_back(std::move(table));
        }
        if (program_.outputs.empty()) {
            for (const auto& [name, direction] : source.param_directions) {
                if (parsePortDirection(direction) == PortDirection::Output) {
                    program_.outputs.push_back(name);
                }
            }
            std::sort(program_.outputs.begin(), program_.outputs.end());
        }

        for (const auto& name : sortedKeys(source.symbols)) {
            const TypeInfo& type = source.symbols.at(name);
            if (hasStructType(type)) continue;
            auto dir = source.param_directions.find(name);
            if (dir != source.param_directions.end()) {
                PortDirection direction = parsePortDirection(dir->second);
                ensurePort(name, direction, type);
                if (direction == PortDirection::Input) program_.inputs.push_back(name);
            } else if (type.is_array) {
                ensureAggregate(name, type);
            } else {
                ensureSignal(name, type);
            }
        }
        std::sort(program_.inputs.begin(), program_.inputs.end());

        connectInputPorts(source);

        std::vector<std::string> target_order;
        std::unordered_map<std::string, std::vector<PendingAssignment>> by_target;
        std::unordered_map<std::string, TypeInfo> target_types;
        for (std::size_t i = 0; i < source.assignments.size(); ++i) {
            const auto& assign = source.assignments[i];
            Operand target = flattenTarget(assign.target);
            if (!by_target.count(target.text)) target_order.push_back(target.text);

            TypeInfo type = target.type.width ? target.type : assign.type;
            if (type.width <= 0 && assign.value) type = assign.value->type;
            type = beirType(type);
            target_types[target.text] = type;
            by_target[target.text].push_back({
                assign.guard ? assign.guard : make_true_guard(),
                assign.value,
                type,
                assign.debug_loc
            });
        }

        for (const auto& target_name : target_order) {
            const auto& assignments = by_target.at(target_name);
            TypeInfo type = target_types[target_name];
            ExprPtr value = buildMuxTree(assignments, type);
            Operand rhs = flattenExpr(value);

            Operation op;
            op.kind = OperationKind::Assign;
            op.operands.push_back(std::move(rhs));
            op.type = type;
            for (const auto& assignment : assignments) {
                if (assignment.debug_loc.valid()) op.source_locs.push_back(assignment.debug_loc);
            }
            setDriver(target_name, std::move(op), false);
        }

        for (const auto& output : source.output_expressions) {
            Operand rhs = flattenExpr(output.expr);
            Signal& signal = ensureSignal(output.name, output.type);
            Operation op;
            op.kind = OperationKind::Assign;
            op.operands.push_back(std::move(rhs));
            op.type = signal.type.width ? signal.type : output.type;
            if (output.expr && output.expr->debug_loc.valid()) {
                op.source_locs.push_back(output.expr->debug_loc);
            }
            setDriver(output.name, std::move(op), false);
        }

        return std::move(program_);
    }

private:
    Program program_;
    std::unordered_map<std::string, std::size_t> port_index_;
    std::unordered_map<std::string, std::size_t> aggregate_index_;
    std::unordered_map<std::string, std::size_t> signal_index_;
    std::size_t next_temp_ = 0;

    Signal& ensureSignal(const std::string& name, TypeInfo type) {
        type = beirType(std::move(type));
        if (type.is_array) {
            throw std::runtime_error("beir scalar signal cannot use array type: " + name);
        }
        auto it = signal_index_.find(name);
        if (it == signal_index_.end()) {
            std::size_t index = program_.signals.size();
            signal_index_.emplace(name, index);
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

    Port& ensurePort(const std::string& name, PortDirection direction, TypeInfo type) {
        type = beirType(std::move(type));
        auto it = port_index_.find(name);
        if (it != port_index_.end()) return program_.ports[it->second];

        std::size_t index = program_.ports.size();
        port_index_.emplace(name, index);
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
        type = beirType(std::move(type));
        auto it = aggregate_index_.find(name);
        if (it != aggregate_index_.end()) return program_.aggregates[it->second];

        std::size_t index = program_.aggregates.size();
        aggregate_index_.emplace(name, index);
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

    void connectInputPorts(const PredicateProgram& source) {
        for (const auto& port : program_.ports) {
            if (port.direction != PortDirection::Input) continue;
            for (std::size_t i = 0; i < port.element_symbols.size(); ++i) {
                Operation op;
                op.kind = OperationKind::PortRead;
                op.type = signalType(port.element_symbols[i]);
                op.operands.push_back(portOperand(port.name, port.type));
                auto loc_it = source.param_debug_locs.find(port.name);
                if (loc_it != source.param_debug_locs.end() && loc_it->second.valid()) {
                    op.source_locs.push_back(loc_it->second);
                }
                if (port.element_symbols.size() > 1) {
                    op.operands.push_back(literalOperand(std::to_string(i), TypeInfo{"int", 32, true}));
                }
                setDriver(port.element_symbols[i], std::move(op), true);
            }
        }
    }

    Operand flattenTarget(const ExprPtr& expr) {
        if (!expr) throw std::runtime_error("beir assignment target is null");
        if (expr->kind == ExprKind::VarRef) {
            if (hasStructType(expr->type)) {
                throw std::runtime_error("beir rejects struct assignment target: " + expr->var_name);
            }
            return symbolOperand(expr->var_name, expr->type);
        }
        if (expr->kind == ExprKind::ArrayAccess && expr->array_base &&
            expr->index && expr->index->kind == ExprKind::Literal) {
            Operand base = flattenTarget(expr->array_base);
            return symbolOperand(base.text + "_" + expr->index->literal_value,
                                 scalarElementType(expr->type));
        }
        throw std::runtime_error("beir assignment target must flatten to a named signal");
    }

    Operand flattenExpr(const ExprPtr& expr) {
        if (!expr) return literalOperand("1", TypeInfo{"bool", 1, false});
        if (expr->kind == ExprKind::Literal) {
            return literalOperand(expr->literal_value, expr->type);
        }
        if (expr->kind == ExprKind::VarRef) {
            if (hasStructType(expr->type)) {
                throw std::runtime_error("beir rejects struct operand: " + expr->var_name);
            }
            return symbolOperand(expr->var_name, expr->type);
        }
        if (expr->kind == ExprKind::ArrayAccess && expr->array_base &&
            expr->index && expr->index->kind == ExprKind::Literal) {
            Operand base = flattenExpr(expr->array_base);
            return symbolOperand(base.text + "_" + expr->index->literal_value,
                                 scalarElementType(expr->type));
        }

        std::string temp_name = makeTempName(tempStemForExpr(expr));
        ensureSignal(temp_name, expr->type);

        Operation op;
        op.kind = operationKindForExpr(expr);
        op.type = expr->type;
        if (expr->debug_loc.valid()) op.source_locs.push_back(expr->debug_loc);

        switch (expr->kind) {
        case ExprKind::BinaryOp:
            op.op = parseBinaryOpCode(expr->op);
            op.operands.push_back(flattenExpr(expr->left));
            op.operands.push_back(flattenExpr(expr->right));
            break;
        case ExprKind::UnaryOp:
            op.op = parseUnaryOpCode(expr->op);
            op.operands.push_back(flattenExpr(expr->operand));
            break;
        case ExprKind::ArrayAccess:
            op.operands.push_back(flattenExpr(expr->array_base));
            op.operands.push_back(flattenExpr(expr->index));
            break;
        case ExprKind::FieldAccess:
            throw std::runtime_error("beir rejects unflattened field access: " + expr->field_name);
        case ExprKind::Call:
            if (expr->intrinsic != IntrinsicKind::DynamicBitAt &&
                expr->intrinsic != IntrinsicKind::DynamicRangeAt &&
                expr->callee != "lookup") {
                throw std::runtime_error("beir unsupported call expression after normalization: " +
                                         expr->callee);
            }
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
        case ExprKind::DynamicWriteSlice:
        case ExprKind::DynamicWriteBit:
            op.operands.push_back(flattenExpr(expr->base));
            op.operands.push_back(flattenExpr(expr->index));
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

    ExprPtr buildMuxTree(const std::vector<PendingAssignment>& assignments,
                         const TypeInfo& type) {
        ExprPtr result = defaultValueFor(type);
        for (const auto& assignment : assignments) {
            if (isTrueGuard(assignment.guard)) {
                result = assignment.value;
                continue;
            }
            TypeInfo mux_type = assignment.type.width > 0 ? assignment.type : type;
            result = make_ite(assignment.guard, assignment.value, result, mux_type);
            result->debug_loc = assignment.debug_loc;
        }
        return result;
    }

    Operand symbolOperand(const std::string& name, TypeInfo type) {
        type = beirType(std::move(type));
        if (type.is_array || aggregate_index_.count(name)) {
            ensureAggregate(name, type);
            Operand operand;
            operand.kind = OperandKind::Aggregate;
            operand.text = name;
            operand.type = std::move(type);
            return operand;
        }

        Signal& signal = ensureSignal(name, type);
        Operand operand;
        operand.kind = OperandKind::Symbol;
        operand.text = signal.name;
        operand.type = signal.type;
        return operand;
    }

    Operand literalOperand(std::string value, TypeInfo type) {
        Operand operand;
        operand.kind = OperandKind::Literal;
        operand.text = std::move(value);
        operand.type = beirType(std::move(type));
        return operand;
    }

    Operand portOperand(std::string value, TypeInfo type) {
        Operand operand;
        operand.kind = OperandKind::Port;
        operand.text = std::move(value);
        operand.type = beirType(std::move(type));
        return operand;
    }

    TypeInfo signalType(const std::string& name) const {
        auto it = signal_index_.find(name);
        if (it == signal_index_.end()) return {};
        return program_.signals[it->second].type;
    }

    void setDriver(const std::string& signal_name, Operation op, bool allow_existing_same_kind) {
        Signal& signal = ensureSignal(signal_name, op.type);
        if (signal.driver) {
            if (allow_existing_same_kind && signal.driver->kind == op.kind) return;
            throw std::runtime_error("beir signal has more than one driver: " + signal_name);
        }
        signal.driver = std::move(op);
    }

    void mergeType(TypeInfo& existing, const TypeInfo& incoming) {
        if (existing.name.empty()) existing.name = incoming.name;
        if (existing.width == 0) existing.width = incoming.width;
        if (!existing.is_signed) existing.is_signed = incoming.is_signed;
        if (!existing.is_hw_int) existing.is_hw_int = incoming.is_hw_int;
        if (existing.hw_kind.empty()) existing.hw_kind = incoming.hw_kind;
    }

    std::string makeTempName(const std::string& stem) {
        std::string clean = sanitizeNamePart(stem);
        while (true) {
            std::string name = "__rtlzz_" + clean + "_" + std::to_string(next_temp_++);
            if (!signal_index_.count(name) && !aggregate_index_.count(name)) return name;
        }
    }
};

static std::string typeText(const TypeInfo& type) {
    std::ostringstream os;
    os << type.name;
    if (type.width > 0) os << " width=" << type.width;
    if (type.is_signed) os << " signed";
    if (type.is_array) {
        os << " array[";
        for (std::size_t i = 0; i < type.array_dims.size(); ++i) {
            if (i) os << "x";
            os << type.array_dims[i];
        }
        if (type.array_dims.empty()) os << type.array_size;
        os << "]";
    }
    if (!type.hw_kind.empty()) os << " kind=" << type.hw_kind;
    return os.str();
}

static void emitNameList(std::ostream& os, const std::vector<std::string>& values) {
    os << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) os << ", ";
        os << values[i];
    }
    os << "]";
}

static void emitLocs(std::ostream& os, const std::vector<DebugLoc>& locs) {
    if (locs.empty()) return;
    os << " locs=[";
    for (std::size_t i = 0; i < locs.size(); ++i) {
        if (i) os << ", ";
        const auto& loc = locs[i];
        os << loc.file << ":" << loc.line << ":" << loc.column;
        if (loc.end_line || loc.end_column) {
            os << "-" << loc.end_line << ":" << loc.end_column;
        }
    }
    os << "]";
}

static void emitOperand(std::ostream& os, const Operand& operand) {
    os << operandKindText(operand.kind) << "(" << operand.text << " : " << typeText(operand.type) << ")";
}

static void emitOperation(std::ostream& os, const Operation& op, const std::string& prefix) {
    os << prefix << "driver " << operationKindText(op.kind);
    if (op.op != OpCode::None) os << " op=\"" << opCodeText(op.op) << "\"";
    os << " : " << typeText(op.type);
    if (op.to_width) os << " to_width=" << op.to_width;
    if (op.hi >= 0 || op.lo >= 0) os << " range=" << op.hi << ":" << op.lo;
    if (op.bit >= 0) os << " bit=" << op.bit;
    if (op.times) os << " times=" << op.times;
    emitLocs(os, op.source_locs);
    os << "\n";
    for (std::size_t i = 0; i < op.operands.size(); ++i) {
        os << prefix << "  operand" << i << " = ";
        emitOperand(os, op.operands[i]);
        os << "\n";
    }
}

} // namespace

Program buildProgram(const PredicateProgram& source) {
    return Builder().build(source);
}

std::string emitText(const Program& program) {
    std::ostringstream os;
    os << "beir v1\n";
    os << "function " << program.function_name << "\n";
    os << "inputs ";
    emitNameList(os, program.inputs);
    os << "\n";
    os << "outputs ";
    emitNameList(os, program.outputs);
    os << "\n\n";

    os << "ports\n";
    for (const auto& port : program.ports) {
        os << "  " << portDirectionText(port.direction) << " " << port.name << " : " << typeText(port.type)
           << " elements=";
        emitNameList(os, port.element_symbols);
        os << "\n";
    }
    os << "\n";

    os << "aggregates\n";
    for (const auto& aggregate : program.aggregates) {
        os << "  " << aggregate.name << " : " << typeText(aggregate.type) << " elements=";
        emitNameList(os, aggregate.element_symbols);
        os << "\n";
    }
    os << "\n";

    os << "lookup_tables\n";
    for (const auto& table : program.lookup_tables) {
        os << "  " << table.name << " = ";
        emitNameList(os, table.values);
        os << "\n";
    }
    os << "\n";

    os << "signals\n";
    for (const auto& signal : program.signals) {
        os << "  signal " << signal.name << " : " << typeText(signal.type);
        if (!signal.port_name.empty()) {
            os << " port=" << signal.port_name << "[" << signal.port_element_index << "]";
        }
        os << "\n";
        if (signal.driver) emitOperation(os, *signal.driver, "    ");
        else os << "    driver <none>\n";
    }

    return os.str();
}

std::string emitText(const PredicateProgram& source) {
    return emitText(buildProgram(source));
}

} // namespace pred::beir
