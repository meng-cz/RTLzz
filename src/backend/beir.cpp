#include "backend/beir.hpp"
#include "backend/beopt.hpp"
#include "predicate/PredicateIR.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pred::beir {

bool DebugInfo::hasSourceLoc() const {
    for (const auto& loc : source_locs) {
        if (loc.valid()) return true;
    }
    return false;
}

namespace {

static bool sameDebugLoc(const DebugLoc& lhs, const DebugLoc& rhs) {
    return lhs.file == rhs.file &&
           lhs.line == rhs.line &&
           lhs.column == rhs.column &&
           lhs.end_line == rhs.end_line &&
           lhs.end_column == rhs.end_column;
}

static bool splitVersionedName(const std::string& name, std::string& base) {
    std::size_t pos = name.rfind('_');
    if (pos == std::string::npos || pos + 1 >= name.size()) return false;
    for (std::size_t i = pos + 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
    }
    base = name.substr(0, pos);
    return !base.empty();
}

} // namespace

void addDebugLoc(DebugInfo& debug, const DebugLoc& loc) {
    constexpr std::size_t kMaxSourceLocsPerDebugInfo = 32;
    if (!loc.valid() || debug.source_locs.size() >= kMaxSourceLocsPerDebugInfo) return;
    for (const auto& existing : debug.source_locs) {
        if (sameDebugLoc(existing, loc)) return;
    }
    debug.source_locs.push_back(loc);
}

void addDebugLocs(DebugInfo& debug, const std::vector<DebugLoc>& locs) {
    for (const auto& loc : locs) addDebugLoc(debug, loc);
}

void addDebugInfoLocs(DebugInfo& debug, const DebugInfo& source) {
    addDebugLocs(debug, source.source_locs);
}

void addOperandDebugLocs(DebugInfo& debug, const Program& program, const Operand& operand) {
    if (operand.kind != OperandKind::Symbol || operand.node == kInvalidNodeId) return;
    const Signal* signal = program.findSignal(operand.node);
    if (!signal) return;
    addDebugInfoLocs(debug, signal->debug);
    if (signal->driver) {
        addDebugInfoLocs(debug, signal->driver->debug);
        addDebugLocs(debug, signal->driver->source_locs);
    }
    std::string base_name;
    if (splitVersionedName(signal->name, base_name)) {
        for (const auto& candidate : program.signals) {
            if (candidate.name != base_name) continue;
            addDebugInfoLocs(debug, candidate.debug);
            if (candidate.driver) {
                addDebugInfoLocs(debug, candidate.driver->debug);
                addDebugLocs(debug, candidate.driver->source_locs);
            }
            break;
        }
    }
}

void addOperandDebugLocs(DebugInfo& debug, const Program& program, const std::vector<Operand>& operands) {
    for (const auto& operand : operands) addOperandDebugLocs(debug, program, operand);
}

bool Operand::Constant::isZero() const {
    for (std::uint64_t limb : limbs) {
        if (limb != 0) return false;
    }
    return true;
}

bool Operand::Constant::isOne() const {
    if (limbs.empty()) return false;
    if (limbs[0] != 1) return false;
    for (std::size_t i = 1; i < limbs.size(); ++i) {
        if (limbs[i] != 0) return false;
    }
    return true;
}

bool Operand::Constant::isAllOnes() const {
    if (width <= 0) return false;
    const std::size_t full_limbs = static_cast<std::size_t>(width / 64);
    const int rem_bits = width % 64;
    for (std::size_t i = 0; i < full_limbs; ++i) {
        if (i >= limbs.size() || limbs[i] != std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
    }
    if (rem_bits == 0) return true;
    const std::uint64_t mask = (std::uint64_t{1} << rem_bits) - 1;
    return full_limbs < limbs.size() && limbs[full_limbs] == mask;
}

bool Operand::Constant::isBoolTrue() const {
    return width == 1 && isOne();
}

bool Operand::Constant::isBoolFalse() const {
    return width == 1 && isZero();
}

bool Operand::Constant::fitsU64() const {
    if (width > 64) return false;
    for (std::size_t i = 1; i < limbs.size(); ++i) {
        if (limbs[i] != 0) return false;
    }
    return true;
}

std::uint64_t Operand::Constant::toU64() const {
    if (!fitsU64()) {
        throw std::runtime_error("beir constant does not fit in uint64_t");
    }
    return limbs.empty() ? 0 : limbs[0];
}

Signal* Program::findSignal(NodeId id) {
    if (id >= signals.size()) return nullptr;
    Signal& candidate = signals[static_cast<std::size_t>(id)];
    return candidate.id == id ? &candidate : nullptr;
}

const Signal* Program::findSignal(NodeId id) const {
    if (id >= signals.size()) return nullptr;
    const Signal& candidate = signals[static_cast<std::size_t>(id)];
    return candidate.id == id ? &candidate : nullptr;
}

Signal& Program::signal(NodeId id) {
    if (Signal* found = findSignal(id)) return *found;
    throw std::runtime_error("beir unknown node id");
}

const Signal& Program::signal(NodeId id) const {
    if (const Signal* found = findSignal(id)) return *found;
    throw std::runtime_error("beir unknown node id");
}

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

static int flattenedArraySize(const ValueType& type) {
    if (!type.array_dims.empty()) {
        int size = 1;
        for (int dim : type.array_dims) size *= dim;
        return size;
    }
    return 0;
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

static ValueType scalarElementType(ValueType type) {
    type.array_dims.clear();
    return type;
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

static ValueType beirType(const TypeInfo& type) {
    if (hasStructType(type)) {
        throw std::runtime_error("beir requires flattened scalar data, got struct type: " +
                                 type.struct_name);
    }
    ValueType out;
    out.width = type.width;
    if (!type.array_dims.empty()) {
        out.array_dims = type.array_dims;
    } else if (type.is_array && type.array_size > 0) {
        out.array_dims = {type.array_size};
    }
    return out;
}

static bool isSignedViewType(const TypeInfo& type) {
    return type.hw_kind == "signed_view";
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

static int constantWidthFor(const ValueType& type, const std::string& text) {
    if (type.width > 0) return type.width;
    if (text == "true" || text == "false") return 1;
    return 64;
}

static std::size_t limbCountForWidth(int width) {
    return width <= 0 ? 0 : static_cast<std::size_t>((width + 63) / 64);
}

static void maskHighBits(Operand::Constant& constant) {
    if (constant.width <= 0 || constant.limbs.empty()) return;
    const int rem_bits = constant.width % 64;
    if (rem_bits == 0) return;
    const std::uint64_t mask = (std::uint64_t{1} << rem_bits) - 1;
    constant.limbs.back() &= mask;
}

static void addSmall(Operand::Constant& constant, std::uint32_t value) {
    unsigned __int128 carry = value;
    for (std::uint64_t& limb : constant.limbs) {
        unsigned __int128 sum = static_cast<unsigned __int128>(limb) + carry;
        limb = static_cast<std::uint64_t>(sum);
        carry = sum >> 64;
        if (!carry) break;
    }
    maskHighBits(constant);
}

static void multiplySmall(Operand::Constant& constant, std::uint32_t factor) {
    unsigned __int128 carry = 0;
    for (std::uint64_t& limb : constant.limbs) {
        unsigned __int128 product = static_cast<unsigned __int128>(limb) * factor + carry;
        limb = static_cast<std::uint64_t>(product);
        carry = product >> 64;
    }
    maskHighBits(constant);
}

static void negateTwosComplement(Operand::Constant& constant) {
    for (std::uint64_t& limb : constant.limbs) limb = ~limb;
    addSmall(constant, 1);
    maskHighBits(constant);
}

static std::string stripIntegerSuffix(std::string text) {
    while (!text.empty()) {
        char ch = text.back();
        if (ch == 'u' || ch == 'U' || ch == 'l' || ch == 'L') text.pop_back();
        else break;
    }
    return text;
}

static int digitValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static Operand::Constant parseConstant(std::string text, const ValueType& type, bool signed_view) {
    Operand::Constant constant;
    constant.width = constantWidthFor(type, text);
    constant.signed_view = signed_view;
    constant.limbs.assign(limbCountForWidth(constant.width), 0);

    if (text == "true" || text == "false") {
        if (text == "true") addSmall(constant, 1);
        return constant;
    }

    text = stripIntegerSuffix(std::move(text));
    if (text.empty()) throw std::runtime_error("beir empty constant literal");

    bool negative = false;
    if (text.front() == '+' || text.front() == '-') {
        negative = text.front() == '-';
        text.erase(text.begin());
    }
    if (text.empty()) throw std::runtime_error("beir malformed constant literal");

    int base = 10;
    std::size_t pos = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        pos = 2;
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        pos = 2;
    } else if (text.size() > 1 && text[0] == '0') {
        base = 8;
        pos = 1;
    }

    bool saw_digit = false;
    for (; pos < text.size(); ++pos) {
        if (text[pos] == '\'') continue;
        int digit = digitValue(text[pos]);
        if (digit < 0 || digit >= base) {
            throw std::runtime_error("beir malformed constant literal: " + text);
        }
        saw_digit = true;
        multiplySmall(constant, static_cast<std::uint32_t>(base));
        addSmall(constant, static_cast<std::uint32_t>(digit));
    }
    if (!saw_digit) throw std::runtime_error("beir malformed constant literal: " + text);
    if (negative) negateTwosComplement(constant);
    maskHighBits(constant);
    return constant;
}

static const char* operandKindText(OperandKind kind) {
    switch (kind) {
    case OperandKind::Symbol: return "symbol";
    case OperandKind::Literal: return "literal";
    case OperandKind::Port: return "port";
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
    case OperationKind::Aggregate: return "aggregate";
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
    return PortDirection::Unknown;
}

static const char* portDirectionText(PortDirection direction) {
    switch (direction) {
    case PortDirection::Input: return "Input";
    case PortDirection::Output: return "Output";
    case PortDirection::Unknown: return "Unknown";
    }
    return "Unknown";
}

static const char* debugOriginText(DebugOrigin origin) {
    switch (origin) {
    case DebugOrigin::Source: return "source";
    case DebugOrigin::Generated: return "generated";
    }
    return "generated";
}

static void addValidLoc(DebugInfo& debug, const DebugLoc& loc) {
    if (loc.valid()) debug.source_locs.push_back(loc);
}

static void addValidLocs(DebugInfo& debug, const std::vector<DebugLoc>& locs) {
    for (const auto& loc : locs) addValidLoc(debug, loc);
}

static DebugInfo sourceDebug(const std::vector<DebugLoc>& locs) {
    DebugInfo debug;
    debug.origin = DebugOrigin::Source;
    debug.reason = "direct source construct";
    addValidLocs(debug, locs);
    return debug;
}

static DebugInfo sourceDebug(const std::vector<DebugLoc>& locs, std::string reason) {
    DebugInfo debug;
    debug.origin = DebugOrigin::Source;
    debug.reason = std::move(reason);
    addValidLocs(debug, locs);
    return debug;
}

static DebugInfo sourceDebug(const DebugLoc& loc, std::string reason = "direct source construct") {
    DebugInfo debug;
    debug.origin = DebugOrigin::Source;
    debug.reason = std::move(reason);
    addValidLoc(debug, loc);
    return debug;
}

static DebugInfo generatedDebug(std::string reason,
                                const std::vector<Operand>& operands = {},
                                const std::vector<DebugLoc>& locs = {}) {
    DebugInfo debug;
    debug.origin = DebugOrigin::Generated;
    debug.reason = std::move(reason);
    addValidLocs(debug, locs);
    for (const auto& operand : operands) {
        if (operand.kind == OperandKind::Symbol && operand.node != kInvalidNodeId) {
            debug.derived_nodes.push_back(operand.node);
        } else if (!operand.text.empty()) {
            debug.derived_names.push_back(operand.text);
        }
    }
    return debug;
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
        for (const auto& [name, direction] : source.param_directions) {
            if (direction == "InOut") {
                throw std::runtime_error("beir rejects InOut port direction: " + name);
            }
        }
        lookup_table_values_ = source.lookup_tables;
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

            TypeInfo type = assign.type;
            if (type.width <= 0 && assign.value) type = assign.value->type;
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
            op.type = beirType(type);
            for (const auto& assignment : assignments) {
                if (assignment.debug_loc.valid()) op.source_locs.push_back(assignment.debug_loc);
            }
            op.debug = op.source_locs.empty()
                ? generatedDebug("assignment merged during predication", op.operands)
                : sourceDebug(op.source_locs);
            setDriver(target_name, std::move(op), false);
        }

        for (const auto& output : source.output_expressions) {
            Operand rhs = flattenExpr(output.expr);
            Signal& signal = ensureSignal(output.name, output.type);
            Operation op;
            op.kind = OperationKind::Assign;
            op.operands.push_back(std::move(rhs));
            op.type = signal.type.width ? signal.type : beirType(output.type);
            if (output.expr && output.expr->debug_loc.valid()) {
                op.source_locs.push_back(output.expr->debug_loc);
            }
            op.debug = op.source_locs.empty()
                ? generatedDebug("output expression assignment", op.operands)
                : sourceDebug(op.source_locs, "output expression from source");
            setDriver(output.name, std::move(op), false);
        }

        finalizeAggregateDrivers();

        return std::move(program_);
    }

private:
    Program program_;
    std::unordered_map<std::string, std::size_t> port_index_;
    std::unordered_map<std::string, NodeId> aggregate_index_;
    std::unordered_map<std::string, NodeId> signal_id_by_name_;
    std::unordered_map<std::string, std::vector<std::string>> lookup_table_values_;
    NodeId next_node_id_ = 0;
    std::size_t next_temp_ = 0;

    Signal& ensureSignal(const std::string& name, const TypeInfo& type) {
        return ensureSignal(name, beirType(type));
    }

    Signal& ensureSignal(const std::string& name, ValueType type) {
        auto it = signal_id_by_name_.find(name);
        if (it == signal_id_by_name_.end()) {
            NodeId id = next_node_id_++;
            signal_id_by_name_.emplace(name, id);
            Signal signal;
            signal.id = id;
            signal.name = name;
            signal.type = std::move(type);
            signal.debug = generatedDebug("signal allocated by BEIR builder");
            program_.signals.push_back(std::move(signal));
            return program_.signals.back();
        }
        Signal& signal = signalById(it->second);
        mergeType(signal.type, type);
        return signal;
    }

    Signal& signalById(NodeId id) {
        return program_.signal(id);
    }

    const Signal& signalById(NodeId id) const {
        return program_.signal(id);
    }

    Port& ensurePort(const std::string& name, PortDirection direction, TypeInfo source_type) {
        ValueType type = beirType(source_type);
        auto it = port_index_.find(name);
        if (it != port_index_.end()) return program_.ports[it->second];

        std::size_t index = program_.ports.size();
        port_index_.emplace(name, index);
        Port port;
        port.name = name;
        port.direction = direction;
        port.type = type;

        ValueType element_type = scalarElementType(type);
        if (type.isArray()) {
            Signal& array_signal = ensureSignal(name, type);
            array_signal.port_name = name;
            array_signal.port_element_index = 0;
            int count = flattenedArraySize(type);
            for (int i = 0; i < count; ++i) {
                std::string element_name = name + "_" + std::to_string(i);
                Signal& element = ensureSignal(element_name, element_type);
                port.element_nodes.push_back(element.id);
                element.port_name = name;
                element.port_element_index = i;
            }
        } else {
            Signal& element = ensureSignal(name, element_type);
            port.element_nodes.push_back(element.id);
            element.port_name = name;
            element.port_element_index = 0;
        }

        program_.ports.push_back(std::move(port));
        return program_.ports.back();
    }

    Signal& ensureAggregate(const std::string& name, TypeInfo source_type) {
        ValueType type = beirType(source_type);
        auto it = aggregate_index_.find(name);
        if (it != aggregate_index_.end()) return signalById(it->second);

        Signal& aggregate_signal = ensureSignal(name, type);
        aggregate_index_.emplace(name, aggregate_signal.id);

        ValueType element_type = scalarElementType(type);
        int count = flattenedArraySize(type);
        for (int i = 0; i < count; ++i) {
            std::string element_name = name + "_" + std::to_string(i);
            ensureSignal(element_name, element_type);
        }

        return aggregate_signal;
    }

    void connectInputPorts(const PredicateProgram& source) {
        for (const auto& port : program_.ports) {
            if (port.direction != PortDirection::Input) continue;
            for (std::size_t i = 0; i < port.element_nodes.size(); ++i) {
                const Signal& element = signalById(port.element_nodes[i]);
                Operation op;
                op.kind = OperationKind::PortRead;
                op.type = element.type;
                op.operands.push_back(portOperand(port.name, port.type));
                auto loc_it = source.param_debug_locs.find(port.name);
                if (loc_it != source.param_debug_locs.end() && loc_it->second.valid()) {
                    op.source_locs.push_back(loc_it->second);
                }
                if (port.element_nodes.size() > 1) {
                    op.operands.push_back(literalOperand(std::to_string(i), TypeInfo{"int", 32, true}));
                }
                op.debug = op.source_locs.empty()
                    ? generatedDebug("read from input port '" + port.name + "'", op.operands)
                    : sourceDebug(op.source_locs, "read from source parameter '" + port.name + "'");
                setDriver(element.name, std::move(op), true);
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
        Signal& temp_signal = ensureSignal(temp_name, expr->type);
        NodeId temp_id = temp_signal.id;
        temp_signal.debug.origin = DebugOrigin::Generated;
        temp_signal.debug.reason = "temporary for " +
            std::string(operationKindText(operationKindForExpr(expr)));
        addValidLoc(temp_signal.debug, expr->debug_loc);

        Operation op;
        op.kind = operationKindForExpr(expr);
        op.type = beirType(expr->type);
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

        op.debug = generatedDebug("lowered " + std::string(operationKindText(op.kind)) +
                                      " expression into temporary '" + temp_name + "'",
                                  op.operands,
                                  op.source_locs);
        signalById(temp_id).debug = op.debug;
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

    NodeId latestElementNode(const std::string& aggregate_name, int index) const {
        std::string stem = aggregate_name + "_" + std::to_string(index);
        NodeId best = kInvalidNodeId;
        int best_version = -1;
        auto base_it = signal_id_by_name_.find(stem);
        if (base_it != signal_id_by_name_.end()) best = base_it->second;
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
        return best;
    }

    bool hasVersionedElement(const std::string& aggregate_name, int index) const {
        std::string stem = aggregate_name + "_" + std::to_string(index);
        for (const auto& signal : program_.signals) {
            std::string base;
            if (endsWithNumber(signal.name, base) && base == stem) return true;
        }
        return false;
    }

    void finalizeAggregateDrivers() {
        for (const auto& [name, id] : aggregate_index_) {
            Signal& aggregate = signalById(id);
            if (aggregate.driver) continue;
            if (!aggregate.type.isArray()) {
                throw std::runtime_error("beir aggregate signal is not array typed: " + name);
            }
            ValueType element_type = scalarElementType(aggregate.type);
            int count = flattenedArraySize(aggregate.type);
            Operation op;
            op.kind = OperationKind::Aggregate;
            op.type = aggregate.type;
            auto init_it = lookup_table_values_.find(name);
            for (int i = 0; i < count; ++i) {
                if (init_it != lookup_table_values_.end() &&
                    i < static_cast<int>(init_it->second.size()) &&
                    !hasVersionedElement(name, i)) {
                    op.operands.push_back(literalOperand(init_it->second[static_cast<std::size_t>(i)],
                                                         TypeInfo{"", element_type.width, true}));
                    continue;
                }
                NodeId element_id = latestElementNode(name, i);
                if (element_id == kInvalidNodeId) {
                    throw std::runtime_error("beir cannot find aggregate element for: " +
                                             name + "[" + std::to_string(i) + "]");
                }
                Operand operand;
                operand.kind = OperandKind::Symbol;
                operand.node = element_id;
                operand.text = signalById(element_id).name;
                operand.type = signalById(element_id).type;
                op.operands.push_back(std::move(operand));
            }
            op.debug = generatedDebug("aggregate array value for '" + name + "'", op.operands);
            aggregate.debug = op.debug;
            aggregate.driver = std::move(op);
        }
    }

    Operand symbolOperand(const std::string& name, TypeInfo type) {
        bool signed_view = isSignedViewType(type);
        if (type.is_array || aggregate_index_.count(name)) {
            auto existing = signal_id_by_name_.find(name);
            if (existing != signal_id_by_name_.end()) {
                Signal& signal = signalById(existing->second);
                if (signal.type.isArray()) {
                    Operand operand;
                    operand.kind = OperandKind::Symbol;
                    operand.node = signal.id;
                    operand.text = signal.name;
                    operand.type = signal.type;
                    operand.signed_view = signed_view;
                    return operand;
                }
            }
            Signal& signal = ensureAggregate(name, type);
            Operand operand;
            operand.kind = OperandKind::Symbol;
            operand.node = signal.id;
            operand.text = signal.name;
            operand.type = signal.type;
            operand.signed_view = signed_view;
            return operand;
        }

        Signal& signal = ensureSignal(name, type);
        Operand operand;
        operand.kind = OperandKind::Symbol;
        operand.node = signal.id;
        operand.text = signal.name;
        operand.type = signal.type;
        operand.signed_view = signed_view;
        return operand;
    }

    Operand literalOperand(std::string value, TypeInfo type) {
        bool signed_view = isSignedViewType(type);
        ValueType storage_type = beirType(type);
        if (type.is_array) {
            Signal& signal = ensureAggregate(value, type);
            Operand operand;
            operand.kind = OperandKind::Symbol;
            operand.node = signal.id;
            operand.text = signal.name;
            operand.type = signal.type;
            operand.signed_view = signed_view;
            return operand;
        }
        Operand operand;
        operand.kind = OperandKind::Literal;
        operand.constant = parseConstant(value, storage_type, signed_view);
        operand.text = std::move(value);
        operand.type = std::move(storage_type);
        operand.signed_view = signed_view;
        return operand;
    }

    Operand portOperand(std::string value, ValueType type) {
        Operand operand;
        operand.kind = OperandKind::Port;
        operand.text = std::move(value);
        operand.type = std::move(type);
        return operand;
    }

    void setDriver(const std::string& signal_name, Operation op, bool allow_existing_same_kind) {
        Signal& signal = ensureSignal(signal_name, op.type);
        validateOperationTypes(op);
        if (signal.driver) {
            if (allow_existing_same_kind && signal.driver->kind == op.kind) return;
            throw std::runtime_error("beir signal has more than one driver: " + signal_name);
        }
        if (op.debug.reason.empty()) {
            op.debug = op.source_locs.empty()
                ? generatedDebug("driver for signal '" + signal_name + "'", op.operands)
                : sourceDebug(op.source_locs);
        }
        signal.debug = op.debug;
        signal.driver = std::move(op);
    }

    void validateOperationTypes(const Operation& op) const {
        auto reject_array_operand = [&](const Operand& operand) {
            if (operand.type.isArray()) {
                throw std::runtime_error("beir operation '" + std::string(operationKindText(op.kind)) +
                                         "' cannot use array operand directly");
            }
        };

        switch (op.kind) {
        case OperationKind::Aggregate:
            if (!op.type.isArray()) {
                throw std::runtime_error("beir aggregate operation must produce array type");
            }
            for (const auto& operand : op.operands) {
                if (operand.type.isArray()) {
                    ValueType expected = op.type;
                    if (expected.array_dims.empty()) {
                        throw std::runtime_error("beir malformed aggregate array type");
                    }
                    expected.array_dims.erase(expected.array_dims.begin());
                    if (operand.type.width != expected.width ||
                        operand.type.array_dims != expected.array_dims) {
                        throw std::runtime_error("beir aggregate operand array dimension mismatch");
                    }
                } else if (operand.type.width != op.type.width) {
                    throw std::runtime_error("beir aggregate operand width mismatch");
                }
            }
            return;
        case OperationKind::Lookup:
        case OperationKind::ArrayAccess:
            if (op.operands.empty() || !op.operands[0].type.isArray()) {
                throw std::runtime_error("beir lookup/array_access requires array base operand");
            }
            for (std::size_t i = 1; i < op.operands.size(); ++i) reject_array_operand(op.operands[i]);
            if (op.type.isArray()) {
                ValueType expected = op.operands[0].type;
                expected.array_dims.erase(expected.array_dims.begin());
                if (op.type.width != expected.width ||
                    op.type.array_dims != expected.array_dims) {
                    throw std::runtime_error("beir lookup/array_access result dimension mismatch");
                }
            }
            return;
        case OperationKind::PortRead:
            for (std::size_t i = 1; i < op.operands.size(); ++i) reject_array_operand(op.operands[i]);
            return;
        case OperationKind::Assign:
            if (op.operands.size() == 1 && op.type.isArray() != op.operands[0].type.isArray()) {
                throw std::runtime_error("beir assign array/scalar type mismatch");
            }
            return;
        default:
            if (op.type.isArray()) {
                throw std::runtime_error("beir operation '" + std::string(operationKindText(op.kind)) +
                                         "' cannot produce array type");
            }
            for (const auto& operand : op.operands) reject_array_operand(operand);
            return;
        }
    }

    void mergeType(ValueType& existing, const ValueType& incoming) {
        if (existing.width == 0) existing.width = incoming.width;
        if (existing.array_dims.empty()) existing.array_dims = incoming.array_dims;
    }

    std::string makeTempName(const std::string& stem) {
        std::string clean = sanitizeNamePart(stem);
        while (true) {
            std::string name = "__rtlzz_" + clean + "_" + std::to_string(next_temp_++);
            if (!signal_id_by_name_.count(name) && !aggregate_index_.count(name)) return name;
        }
    }
};

} // namespace

bool sameType(const ValueType& lhs, const ValueType& rhs) {
    return lhs.width == rhs.width && lhs.array_dims == rhs.array_dims;
}

bool isCommutativeOp(OperationKind kind, OpCode op) {
    if (kind != OperationKind::Binary) return false;
    switch (op) {
    case OpCode::Add:
    case OpCode::Mul:
    case OpCode::BitAnd:
    case OpCode::BitOr:
    case OpCode::BitXor:
    case OpCode::LogicAnd:
    case OpCode::LogicOr:
    case OpCode::Eq:
    case OpCode::Ne:
        return true;
    default:
        return false;
    }
}

void hashCombine(std::uint64_t& seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

namespace {

static int factsWidthOf(const ValueType& type) {
    return type.width > 0 ? type.width : 1;
}

static std::size_t factsLimbCount(int width) {
    return width <= 0 ? 0 : static_cast<std::size_t>((width + 63) / 64);
}

static void factsTrim(std::vector<std::uint64_t>& limbs, int width) {
    limbs.resize(factsLimbCount(width), 0);
    if (limbs.empty() || width % 64 == 0) return;
    limbs.back() &= ((std::uint64_t{1} << (width % 64)) - 1);
}

static std::vector<std::uint64_t> factsZeros(int width) {
    return std::vector<std::uint64_t>(factsLimbCount(width), 0);
}

static bool factsGetBit(const std::vector<std::uint64_t>& limbs, int bit) {
    if (bit < 0) return false;
    std::size_t limb = static_cast<std::size_t>(bit / 64);
    return limb < limbs.size() && ((limbs[limb] >> (bit % 64)) & 1ULL) != 0;
}

static void factsSetBit(std::vector<std::uint64_t>& limbs, int bit) {
    if (bit < 0) return;
    std::size_t limb = static_cast<std::size_t>(bit / 64);
    if (limb < limbs.size()) limbs[limb] |= (1ULL << (bit % 64));
}

static std::vector<std::uint64_t> factsBitNot(std::vector<std::uint64_t> value, int width) {
    for (auto& limb : value) limb = ~limb;
    factsTrim(value, width);
    return value;
}

static std::vector<std::uint64_t> factsSliceBits(const std::vector<std::uint64_t>& value, int lo, int width) {
    std::vector<std::uint64_t> out = factsZeros(width);
    for (int bit = 0; bit < width; ++bit) {
        if (factsGetBit(value, lo + bit)) factsSetBit(out, bit);
    }
    return out;
}

static Operand::Constant factsMakeConstant(std::vector<std::uint64_t> limbs, int width, bool signed_view = false) {
    Operand::Constant constant;
    constant.width = width;
    constant.signed_view = signed_view;
    constant.limbs = std::move(limbs);
    factsTrim(constant.limbs, width);
    return constant;
}

static ValueFacts factsUnknown(int width) {
    ValueFacts facts;
    facts.valid = true;
    facts.width = width;
    facts.known_zero = factsZeros(width);
    facts.known_one = factsZeros(width);
    return facts;
}

static ValueFacts factsFromConstant(const Operand::Constant& constant, int width) {
    ValueFacts facts;
    facts.valid = true;
    facts.width = width;
    facts.constant = true;
    facts.value = factsMakeConstant(constant.limbs, width, constant.signed_view);
    facts.known_one = facts.value.limbs;
    facts.known_zero = factsBitNot(facts.known_one, width);
    return facts;
}

static ValueFacts factsZExt(const ValueFacts& src, int out_width) {
    if (src.width <= 0) return factsUnknown(out_width);
    ValueFacts out = factsUnknown(out_width);
    int copy_width = std::min(src.width, out_width);
    out.known_zero = factsSliceBits(src.known_zero, 0, copy_width);
    out.known_one = factsSliceBits(src.known_one, 0, copy_width);
    out.known_zero.resize(factsLimbCount(out_width), 0);
    out.known_one.resize(factsLimbCount(out_width), 0);
    for (int bit = src.width; bit < out_width; ++bit) factsSetBit(out.known_zero, bit);
    factsTrim(out.known_zero, out_width);
    factsTrim(out.known_one, out_width);
    if (src.constant) out = factsFromConstant(factsMakeConstant(src.value.limbs, copy_width, src.value.signed_view), out_width);
    return out;
}

static ValueFacts factsTrunc(const ValueFacts& src, int out_width) {
    if (src.width <= 0) return factsUnknown(out_width);
    ValueFacts out = factsUnknown(out_width);
    out.known_zero = factsSliceBits(src.known_zero, 0, out_width);
    out.known_one = factsSliceBits(src.known_one, 0, out_width);
    if (src.constant) out = factsFromConstant(factsMakeConstant(src.value.limbs, out_width, src.value.signed_view), out_width);
    return out;
}

static ValueFacts factsSlice(const ValueFacts& src, int lo, int out_width) {
    if (src.width <= 0) return factsUnknown(out_width);
    ValueFacts out = factsUnknown(out_width);
    out.known_zero = factsSliceBits(src.known_zero, lo, out_width);
    out.known_one = factsSliceBits(src.known_one, lo, out_width);
    if (src.constant) out = factsFromConstant(factsMakeConstant(factsSliceBits(src.value.limbs, lo, out_width), out_width, src.value.signed_view), out_width);
    return out;
}

static ValueFacts factsBitSelect(const ValueFacts& src, int bit) {
    ValueFacts out = factsUnknown(1);
    if (src.width <= 0) return out;
    if (factsGetBit(src.known_zero, bit)) factsSetBit(out.known_zero, 0);
    if (factsGetBit(src.known_one, bit)) factsSetBit(out.known_one, 0);
    if (src.constant) out = factsFromConstant(factsMakeConstant({factsGetBit(src.value.limbs, bit) ? 1ULL : 0ULL}, 1, src.value.signed_view), 1);
    return out;
}

static ValueFacts factsInferOperation(const Operation& op, const Program& program) {
    int width = factsWidthOf(op.type);
    ValueFacts unknown_operand = factsUnknown(width);
    std::vector<ValueFacts> operands;
    operands.reserve(op.operands.size());
    for (const auto& operand : op.operands) {
        if (operand.kind == OperandKind::Literal) operands.push_back(factsFromConstant(operand.constant, factsWidthOf(operand.type)));
        else if (operand.kind == OperandKind::Symbol) {
            const Signal* signal = program.findSignal(operand.node);
            operands.push_back(signal ? signal->value : unknown_operand);
        } else {
            operands.push_back(unknown_operand);
        }
    }

    if (op.kind == OperationKind::Assign || op.kind == OperationKind::Cast) {
        if (operands.empty()) return factsUnknown(width);
        if (width >= operands[0].width) return factsZExt(operands[0], width);
        return factsTrunc(operands[0], width);
    }
    if (op.kind == OperationKind::ZExt) return operands.empty() ? factsUnknown(width) : factsZExt(operands[0], width);
    if (op.kind == OperationKind::Trunc) return operands.empty() ? factsUnknown(width) : factsTrunc(operands[0], width);
    if (op.kind == OperationKind::Slice) return operands.empty() ? factsUnknown(width) : factsSlice(operands[0], op.lo, width);
    if (op.kind == OperationKind::BitSelect) return operands.empty() ? factsUnknown(1) : factsBitSelect(operands[0], op.bit);
    if (op.kind == OperationKind::Unary && !operands.empty() && op.op == OpCode::BitNot) {
        ValueFacts out = factsUnknown(operands[0].width);
        out.known_zero = operands[0].known_one;
        out.known_one = operands[0].known_zero;
        if (operands[0].constant) out = factsFromConstant(factsMakeConstant(factsBitNot(operands[0].value.limbs, operands[0].width), operands[0].width, operands[0].value.signed_view), operands[0].width);
        return out;
    }
    if (op.kind == OperationKind::Binary && operands.size() >= 2 &&
        (op.op == OpCode::BitAnd || op.op == OpCode::BitOr || op.op == OpCode::BitXor)) {
        ValueFacts out = factsUnknown(width);
        for (std::size_t i = 0; i < out.known_zero.size(); ++i) {
            std::uint64_t lz = i < operands[0].known_zero.size() ? operands[0].known_zero[i] : 0;
            std::uint64_t rz = i < operands[1].known_zero.size() ? operands[1].known_zero[i] : 0;
            std::uint64_t lo = i < operands[0].known_one.size() ? operands[0].known_one[i] : 0;
            std::uint64_t ro = i < operands[1].known_one.size() ? operands[1].known_one[i] : 0;
            if (op.op == OpCode::BitAnd) {
                out.known_zero[i] = lz | rz;
                out.known_one[i] = lo & ro;
            } else if (op.op == OpCode::BitOr) {
                out.known_zero[i] = lz & rz;
                out.known_one[i] = lo | ro;
            } else {
                out.known_zero[i] = (lz & rz) | (lo & ro);
                out.known_one[i] = (lz & ro) | (lo & rz);
            }
        }
        factsTrim(out.known_zero, width);
        factsTrim(out.known_one, width);
        return out;
    }
    if ((op.kind == OperationKind::ReduceOr || op.kind == OperationKind::ReduceAnd) && !operands.empty()) {
        ValueFacts out = factsUnknown(1);
        bool any_known = false;
        bool all_known = true;
        for (int bit = 0; bit < operands[0].width; ++bit) {
            if (op.kind == OperationKind::ReduceOr) {
                any_known = any_known || factsGetBit(operands[0].known_one, bit);
                all_known = all_known && factsGetBit(operands[0].known_zero, bit);
            } else {
                any_known = any_known || factsGetBit(operands[0].known_zero, bit);
                all_known = all_known && factsGetBit(operands[0].known_one, bit);
            }
        }
        if (any_known) return factsFromConstant(factsMakeConstant({op.kind == OperationKind::ReduceOr ? 1ULL : 0ULL}, 1), 1);
        if (all_known) return factsFromConstant(factsMakeConstant({op.kind == OperationKind::ReduceOr ? 0ULL : 1ULL}, 1), 1);
        return out;
    }
    return factsUnknown(width);
}

static std::vector<NodeId> factsTopologicalOrder(const Program& program) {
    std::vector<std::vector<NodeId>> users(program.signals.size());
    std::vector<std::size_t> indegree(program.signals.size(), 0);
    for (const auto& signal : program.signals) {
        if (signal.id >= program.signals.size()) throw std::runtime_error("BEIR facts analysis requires dense NodeId indices");
        if (!signal.driver) continue;
        for (const auto& operand : signal.driver->operands) {
            if (operand.kind != OperandKind::Symbol) continue;
            if (operand.node >= program.signals.size() || !program.findSignal(operand.node)) {
                throw std::runtime_error("BEIR facts analysis found dependency on unknown node");
            }
            users[operand.node].push_back(signal.id);
            ++indegree[signal.id];
        }
    }
    std::queue<NodeId> ready;
    for (const auto& signal : program.signals) {
        if (indegree[signal.id] == 0) ready.push(signal.id);
    }
    std::vector<NodeId> order;
    while (!ready.empty()) {
        NodeId id = ready.front();
        ready.pop();
        order.push_back(id);
        for (NodeId user : users[id]) {
            if (--indegree[user] == 0) ready.push(user);
        }
    }
    if (order.size() != program.signals.size()) {
        throw std::runtime_error("BEIR facts analysis requires an acyclic signal dependency graph");
    }
    return order;
}

} // namespace

bool TypeSignature::operator==(const TypeSignature& other) const {
    return width == other.width && array_dims == other.array_dims;
}

bool TypeSignature::operator<(const TypeSignature& other) const {
    if (width != other.width) return width < other.width;
    return array_dims < other.array_dims;
}

std::size_t TypeSignatureHash::operator()(const TypeSignature& sig) const {
    std::uint64_t seed = static_cast<std::uint64_t>(sig.width);
    for (int dim : sig.array_dims) hashCombine(seed, static_cast<std::uint64_t>(dim));
    return static_cast<std::size_t>(seed);
}

bool ConstantSignature::operator==(const ConstantSignature& other) const {
    return width == other.width &&
           signed_view == other.signed_view &&
           limbs == other.limbs;
}

bool ConstantSignature::operator<(const ConstantSignature& other) const {
    if (width != other.width) return width < other.width;
    if (signed_view != other.signed_view) return signed_view < other.signed_view;
    return limbs < other.limbs;
}

bool OperandSignature::operator==(const OperandSignature& other) const {
    return kind == other.kind &&
           node == other.node &&
           text_id == other.text_id &&
           type == other.type &&
           signed_view == other.signed_view &&
           constant == other.constant;
}

bool OperandSignature::operator<(const OperandSignature& other) const {
    if (kind != other.kind) return static_cast<int>(kind) < static_cast<int>(other.kind);
    if (node != other.node) return node < other.node;
    if (text_id != other.text_id) return text_id < other.text_id;
    if (!(type == other.type)) return type < other.type;
    if (signed_view != other.signed_view) return signed_view < other.signed_view;
    return constant < other.constant;
}

std::size_t OperandSignatureHash::operator()(const OperandSignature& sig) const {
    std::uint64_t seed = static_cast<std::uint64_t>(sig.kind);
    hashCombine(seed, sig.node);
    hashCombine(seed, sig.text_id);
    hashCombine(seed, TypeSignatureHash{}(sig.type));
    hashCombine(seed, sig.signed_view ? 1 : 0);
    hashCombine(seed, static_cast<std::uint64_t>(sig.constant.width));
    hashCombine(seed, sig.constant.signed_view ? 1 : 0);
    for (std::uint64_t limb : sig.constant.limbs) hashCombine(seed, limb);
    return static_cast<std::size_t>(seed);
}

bool OperationSignature::operator==(const OperationSignature& other) const {
    return kind == other.kind &&
           op == other.op &&
           type == other.type &&
           to_width == other.to_width &&
           hi == other.hi &&
           lo == other.lo &&
           bit == other.bit &&
           times == other.times &&
           operands == other.operands;
}

std::size_t OperationSignatureHash::operator()(const OperationSignature& sig) const {
    std::uint64_t seed = static_cast<std::uint64_t>(sig.kind);
    hashCombine(seed, static_cast<std::uint64_t>(sig.op));
    hashCombine(seed, TypeSignatureHash{}(sig.type));
    hashCombine(seed, static_cast<std::uint64_t>(sig.to_width));
    hashCombine(seed, static_cast<std::uint64_t>(sig.hi));
    hashCombine(seed, static_cast<std::uint64_t>(sig.lo));
    hashCombine(seed, static_cast<std::uint64_t>(sig.bit));
    hashCombine(seed, static_cast<std::uint64_t>(sig.times));
    OperandSignatureHash operand_hash;
    for (const auto& operand : sig.operands) hashCombine(seed, operand_hash(operand));
    return static_cast<std::size_t>(seed);
}

MutableProgram::MutableProgram(Program program) : program_(std::move(program)) {
    rebuildObservableIds();
}

Program& MutableProgram::program() {
    return program_;
}

const Program& MutableProgram::program() const {
    return program_;
}

Program MutableProgram::finish() {
    return std::move(program_);
}

bool MutableProgram::isObservable(const Signal& signal) const {
    return observable_ids_.count(signal.id) != 0;
}

void MutableProgram::markValueFactsDirty() {
    value_facts_dirty_ = true;
    for (auto& signal : program_.signals) signal.value = ValueFacts{};
}

void MutableProgram::ensureValueFacts() {
    if (!value_facts_dirty_) return;
    analyzeValueFacts();
    value_facts_dirty_ = false;
}

void MutableProgram::analyzeValueFacts() {
    for (auto& signal : program_.signals) signal.value = ValueFacts{};

    std::vector<bool> resolved(program_.signals.size(), false);
    for (NodeId id : factsTopologicalOrder(program_)) {
        Signal& signal = program_.signal(id);
        if (signal.driver) {
            for (const auto& operand : signal.driver->operands) {
                if (operand.kind != OperandKind::Symbol) continue;
                if (operand.node >= resolved.size() || !resolved[operand.node]) {
                    throw std::runtime_error("BEIR facts analysis encountered unresolved dependency before node #" +
                                             std::to_string(id));
                }
            }
            ValueFacts facts = factsInferOperation(*signal.driver, program_);
            facts.width = factsWidthOf(signal.type);
            factsTrim(facts.known_zero, facts.width);
            factsTrim(facts.known_one, facts.width);
            signal.value = std::move(facts);
        } else {
            signal.value = factsUnknown(factsWidthOf(signal.type));
        }
        resolved[id] = true;
    }
}

void MutableProgram::rebuildObservableIds() {
    observable_ids_.clear();
    for (const auto& signal : program_.signals) {
        if (!signal.port_name.empty()) observable_ids_.insert(signal.id);
    }
    for (const auto& output : program_.outputs) {
        for (const auto& signal : program_.signals) {
            if (signal.name == output) {
                observable_ids_.insert(signal.id);
                break;
            }
        }
    }
}

bool MutableProgram::isCseCandidate(const Operation& op) const {
    if (op.kind == OperationKind::Assign || op.kind == OperationKind::PortRead) return false;
    if (op.kind == OperationKind::DynamicWriteSlice ||
        op.kind == OperationKind::DynamicWriteBit ||
        op.kind == OperationKind::WriteSlice ||
        op.kind == OperationKind::WriteBit) {
        return false;
    }
    return true;
}

std::uint64_t MutableProgram::internText(const std::string& text) {
    if (text.empty()) return 0;
    auto it = text_ids_.find(text);
    if (it != text_ids_.end()) return it->second;
    std::uint64_t id = next_text_id_++;
    text_ids_.emplace(text, id);
    return id;
}

TypeSignature MutableProgram::typeSignature(const ValueType& type) const {
    TypeSignature sig;
    sig.width = type.width;
    sig.array_dims = type.array_dims;
    return sig;
}

ConstantSignature MutableProgram::constantSignature(const Operand::Constant& constant) const {
    ConstantSignature sig;
    sig.width = constant.width;
    sig.signed_view = constant.signed_view;
    sig.limbs = constant.limbs;
    return sig;
}

OperandSignature MutableProgram::operandSignature(const Operand& operand) {
    OperandSignature sig;
    sig.kind = operand.kind;
    sig.node = operand.node;
    sig.text_id = internText(operand.text);
    sig.type = typeSignature(operand.type);
    sig.signed_view = operand.signed_view;
    if (operand.kind == OperandKind::Literal) {
        sig.constant = constantSignature(operand.constant);
    }
    return sig;
}

OperationSignature MutableProgram::operationSignature(const Operation& op) {
    OperationSignature sig;
    sig.kind = op.kind;
    sig.op = op.op;
    sig.type = typeSignature(op.type);
    sig.to_width = op.to_width;
    sig.hi = op.hi;
    sig.lo = op.lo;
    sig.bit = op.bit;
    sig.times = op.times;
    sig.operands.reserve(op.operands.size());
    for (const auto& operand : op.operands) {
        sig.operands.push_back(operandSignature(operand));
    }
    if (isCommutativeOp(op.kind, op.op) && sig.operands.size() == 2 &&
        sig.operands[1] < sig.operands[0]) {
        std::swap(sig.operands[0], sig.operands[1]);
    }
    return sig;
}

Operand MutableProgram::resolveOperand(Operand operand, const std::unordered_map<NodeId, Operand>& aliases) const {
    std::unordered_set<NodeId> seen;
    bool use_signed_view = operand.signed_view;
    while (operand.kind == OperandKind::Symbol) {
        auto it = aliases.find(operand.node);
        if (it == aliases.end()) break;
        if (!seen.insert(operand.node).second) break;
        use_signed_view = use_signed_view || operand.signed_view;
        operand = it->second;
    }
    if (use_signed_view) {
        operand.signed_view = true;
        if (operand.kind == OperandKind::Literal) operand.constant.signed_view = true;
    }
    return operand;
}

bool MutableProgram::replaceAliases(const std::unordered_map<NodeId, Operand>& aliases) {
    bool changed = false;
    for (auto& signal : program_.signals) {
        if (!signal.driver) continue;
        for (auto& operand : signal.driver->operands) {
            Operand resolved = resolveOperand(operand, aliases);
            if (!(operandSignature(resolved) == operandSignature(operand))) {
                operand = std::move(resolved);
                changed = true;
            }
        }
    }
    if (changed) markValueFactsDirty();
    return changed;
}

void MutableProgram::remapDebug(DebugInfo& debug, const std::vector<NodeId>& remap) {
    std::vector<NodeId> out;
    out.reserve(debug.derived_nodes.size());
    for (NodeId old_id : debug.derived_nodes) {
        if (old_id < remap.size() && remap[old_id] != kInvalidNodeId) out.push_back(remap[old_id]);
    }
    debug.derived_nodes = std::move(out);
}

void MutableProgram::remapOperand(Operand& operand, const std::vector<NodeId>& remap) {
    if (operand.kind != OperandKind::Symbol) return;
    if (operand.node < remap.size() && remap[operand.node] != kInvalidNodeId) {
        operand.node = remap[operand.node];
    }
}

void MutableProgram::compact(const std::unordered_set<NodeId>& live) {
    std::vector<NodeId> remap(program_.signals.size(), kInvalidNodeId);
    std::vector<Signal> compacted;
    compacted.reserve(live.size());
    for (const auto& signal : program_.signals) {
        if (!live.count(signal.id)) continue;
        NodeId new_id = static_cast<NodeId>(compacted.size());
        remap[signal.id] = new_id;
        compacted.push_back(signal);
        compacted.back().id = new_id;
    }

    auto remap_node_list = [&](std::vector<NodeId>& nodes) {
        std::vector<NodeId> out;
        out.reserve(nodes.size());
        for (NodeId old_id : nodes) {
            if (old_id < remap.size() && remap[old_id] != kInvalidNodeId) out.push_back(remap[old_id]);
        }
        nodes = std::move(out);
    };

    for (auto& port : program_.ports) remap_node_list(port.element_nodes);

    for (auto& signal : compacted) {
        remapDebug(signal.debug, remap);
        if (!signal.driver) continue;
        remapDebug(signal.driver->debug, remap);
        for (auto& operand : signal.driver->operands) remapOperand(operand, remap);
    }
    program_.signals = std::move(compacted);
    markValueFactsDirty();
    rebuildObservableIds();
}

namespace {

static std::string typeText(const ValueType& type) {
    std::ostringstream os;
    os << "bits";
    if (type.width > 0) os << "<" << type.width << ">";
    if (!type.array_dims.empty()) {
        os << " array[";
        for (std::size_t i = 0; i < type.array_dims.size(); ++i) {
            if (i) os << "x";
            os << type.array_dims[i];
        }
        os << "]";
    }
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

static void emitNodeList(std::ostream& os, const Program& program, const std::vector<NodeId>& values) {
    os << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) os << ", ";
        os << "#" << values[i];
        if (const Signal* signal = program.findSignal(values[i])) {
            os << ":" << signal->name;
        }
    }
    os << "]";
}

static void emitConstant(std::ostream& os, const Operand::Constant& constant) {
    os << "const(width=" << constant.width
       << ", signed_view=" << (constant.signed_view ? "true" : "false")
       << ", limbs=[";
    for (std::size_t i = 0; i < constant.limbs.size(); ++i) {
        if (i) os << ", ";
        os << constant.limbs[i];
    }
    os << "])";
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

static void emitDebugInfo(std::ostream& os, const DebugInfo& debug) {
    os << " debug_origin=" << debugOriginText(debug.origin);
    if (!debug.reason.empty()) os << " debug_reason=\"" << debug.reason << "\"";
    if (!debug.derived_nodes.empty()) {
        os << " derived_nodes=[";
        for (std::size_t i = 0; i < debug.derived_nodes.size(); ++i) {
            if (i) os << ", ";
            os << "#" << debug.derived_nodes[i];
        }
        os << "]";
    }
    if (!debug.derived_names.empty()) {
        os << " derived_names=[";
        for (std::size_t i = 0; i < debug.derived_names.size(); ++i) {
            if (i) os << ", ";
            os << debug.derived_names[i];
        }
        os << "]";
    }
}

static void emitOperand(std::ostream& os, const Program& program, const Operand& operand) {
    switch (operand.kind) {
    case OperandKind::Symbol:
        os << "symbol(#" << operand.node;
        if (const Signal* signal = program.findSignal(operand.node)) {
            os << " " << signal->name;
        }
        os << " : " << typeText(operand.type);
        if (operand.signed_view) os << " signed_view";
        os << ")";
        return;
    case OperandKind::Literal:
        emitConstant(os, operand.constant);
        os << " : " << typeText(operand.type);
        if (operand.signed_view) os << " signed_view";
        return;
    case OperandKind::Port:
        os << operandKindText(operand.kind) << "(" << operand.text << " : "
           << typeText(operand.type) << ")";
        if (operand.signed_view) os << " signed_view";
        return;
    }
}

static void emitOperation(std::ostream& os, const Program& program, const Operation& op, const std::string& prefix) {
    os << prefix << "driver " << operationKindText(op.kind);
    if (op.op != OpCode::None) os << " op=\"" << opCodeText(op.op) << "\"";
    os << " : " << typeText(op.type);
    if (op.to_width) os << " to_width=" << op.to_width;
    if (op.hi >= 0 || op.lo >= 0) os << " range=" << op.hi << ":" << op.lo;
    if (op.bit >= 0) os << " bit=" << op.bit;
    if (op.times) os << " times=" << op.times;
    emitLocs(os, op.source_locs);
    emitDebugInfo(os, op.debug);
    os << "\n";
    for (std::size_t i = 0; i < op.operands.size(); ++i) {
        os << prefix << "  operand" << i << " = ";
        emitOperand(os, program, op.operands[i]);
        os << "\n";
    }
}

} // namespace

Program buildProgram(const PredicateProgram& source, bool optimize) {
    Program program = Builder().build(source);
    return optimize ? opt::optimizeProgram(std::move(program)) : program;
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
        emitNodeList(os, program, port.element_nodes);
        os << "\n";
    }
    os << "\n";

    os << "signals\n";
    for (const auto& signal : program.signals) {
        os << "  signal #" << signal.id << " " << signal.name << " : " << typeText(signal.type);
        if (!signal.port_name.empty()) {
            os << " port=" << signal.port_name << "[" << signal.port_element_index << "]";
        }
        emitDebugInfo(os, signal.debug);
        os << "\n";
        if (signal.driver) emitOperation(os, program, *signal.driver, "    ");
        else os << "    driver <none>\n";
    }

    return os.str();
}

std::string emitText(const PredicateProgram& source) {
    return emitText(buildProgram(source));
}

} // namespace pred::beir
