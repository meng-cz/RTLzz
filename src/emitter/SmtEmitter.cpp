#include "emitter/SmtEmitter.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred {

namespace {

struct SmtPrintState {
    static thread_local int depth;
    static thread_local int nodes;
    static thread_local std::vector<std::string>* assumptions;
    static thread_local const std::unordered_map<std::string, TypeInfo>* var_types;
};

thread_local int SmtPrintState::depth = 0;
thread_local int SmtPrintState::nodes = 0;
thread_local std::vector<std::string>* SmtPrintState::assumptions = nullptr;
thread_local const std::unordered_map<std::string, TypeInfo>* SmtPrintState::var_types = nullptr;

struct SmtPrintFrame {
    bool root = false;
    SmtPrintFrame() {
        root = SmtPrintState::depth == 0;
        if (root) SmtPrintState::nodes = 0;
        ++SmtPrintState::depth;
    }
    ~SmtPrintFrame() {
        --SmtPrintState::depth;
        if (root) SmtPrintState::nodes = 0;
    }
};

struct SmtAssumptionScope {
    std::vector<std::string>* previous = nullptr;
    explicit SmtAssumptionScope(std::vector<std::string>& assumptions) {
        previous = SmtPrintState::assumptions;
        SmtPrintState::assumptions = &assumptions;
    }
    ~SmtAssumptionScope() {
        SmtPrintState::assumptions = previous;
    }
};

struct SmtVarTypeScope {
    const std::unordered_map<std::string, TypeInfo>* previous = nullptr;
    explicit SmtVarTypeScope(const std::unordered_map<std::string, TypeInfo>& var_types) {
        previous = SmtPrintState::var_types;
        SmtPrintState::var_types = &var_types;
    }
    ~SmtVarTypeScope() {
        SmtPrintState::var_types = previous;
    }
};

static bool isBoolType(const TypeInfo& type) {
    return type.hw_kind == "bool" || type.name == "bool";
}

static bool isSignedBitVectorType(const TypeInfo& type) {
    return !isBoolType(type) && (type.is_signed || type.hw_kind == "signed_view");
}

static bool sameSmtType(const TypeInfo& a, const TypeInfo& b) {
    return a.width == b.width && isBoolType(a) == isBoolType(b);
}

static bool boolBitVec1Compatible(const TypeInfo& a, const TypeInfo& b) {
    return a.width == 1 && b.width == 1 && isBoolType(a) != isBoolType(b);
}

static void maskToWidth(std::vector<unsigned char>& bytes, int width) {
    if (bytes.empty()) return;
    int extra_bits = static_cast<int>(bytes.size() * 8) - width;
    if (extra_bits > 0) {
        unsigned char mask = static_cast<unsigned char>(0xffu >> extra_bits);
        bytes.back() &= mask;
    }
}

static bool isZeroBytes(const std::vector<unsigned char>& bytes) {
    for (auto b : bytes) {
        if (b != 0) return false;
    }
    return true;
}

static void multiplyAddModulo(std::vector<unsigned char>& bytes, int base, int digit, int width) {
    unsigned int carry = static_cast<unsigned int>(digit);
    for (auto& byte : bytes) {
        unsigned int value = static_cast<unsigned int>(byte) * static_cast<unsigned int>(base) + carry;
        byte = static_cast<unsigned char>(value & 0xffu);
        carry = value >> 8;
    }
    maskToWidth(bytes, width);
}

static void negateModulo(std::vector<unsigned char>& bytes, int width) {
    if (isZeroBytes(bytes)) return;
    for (auto& byte : bytes) byte = static_cast<unsigned char>(~byte);
    unsigned int carry = 1;
    for (auto& byte : bytes) {
        unsigned int value = static_cast<unsigned int>(byte) + carry;
        byte = static_cast<unsigned char>(value & 0xffu);
        carry = value >> 8;
        if (!carry) break;
    }
    maskToWidth(bytes, width);
}

static std::string bytesToDecimal(std::vector<unsigned char> bytes) {
    if (isZeroBytes(bytes)) return "0";
    std::string out;
    while (!isZeroBytes(bytes)) {
        unsigned int rem = 0;
        for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
            unsigned int value = (rem << 8) | static_cast<unsigned int>(*it);
            *it = static_cast<unsigned char>(value / 10);
            rem = value % 10;
        }
        out.push_back(static_cast<char>('0' + rem));
    }
    std::reverse(out.begin(), out.end());
    return out;
}

static std::vector<unsigned char> parseIntegerLiteralModulo(const std::string& value, int width) {
    if (width <= 0) {
        throw std::runtime_error("SmtEmitter: bit-vector literal width must be known");
    }
    if (value.empty()) {
        throw std::runtime_error("SmtEmitter: empty numeric literal");
    }
    std::vector<unsigned char> bytes(static_cast<size_t>((width + 7) / 8), 0);
    size_t pos = 0;
    size_t end = value.size();
    bool negative = false;
    if (value[pos] == '+' || value[pos] == '-') {
        negative = value[pos] == '-';
        ++pos;
    }
    size_t suffix_begin = end;
    while (suffix_begin > pos &&
           (value[suffix_begin - 1] == 'u' || value[suffix_begin - 1] == 'U' ||
            value[suffix_begin - 1] == 'l' || value[suffix_begin - 1] == 'L')) {
        --suffix_begin;
    }
    std::string suffix = value.substr(suffix_begin);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (!(suffix.empty() || suffix == "u" || suffix == "l" || suffix == "ul" ||
          suffix == "lu" || suffix == "ll" || suffix == "ull" || suffix == "llu")) {
        throw std::runtime_error("SmtEmitter: unsupported integer literal suffix in '" + value + "'");
    }
    end = suffix_begin;
    int base = 10;
    if (pos + 1 < end && value[pos] == '0' &&
        (value[pos + 1] == 'x' || value[pos + 1] == 'X')) {
        base = 16;
        pos += 2;
    } else if (pos + 1 < end && value[pos] == '0' &&
               (value[pos + 1] == 'b' || value[pos + 1] == 'B')) {
        base = 2;
        pos += 2;
    } else if (pos + 1 < end && value[pos] == '0') {
        base = 8;
        pos += 1;
    }
    if (pos >= end) {
        if (base == 8) return bytes;
        throw std::runtime_error("SmtEmitter: unsupported numeric literal '" + value + "'");
    }
    for (; pos < end; ++pos) {
        char c = value[pos];
        if (c == '\'' || c == '_') continue;
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') digit = 10 + (c - 'A');
        else {
            throw std::runtime_error("SmtEmitter: unsupported numeric literal '" + value + "'");
        }
        if (digit < 0 || digit >= base) {
            throw std::runtime_error("SmtEmitter: digit out of range in literal '" + value + "'");
        }
        multiplyAddModulo(bytes, base, digit, width);
    }
    if (negative) negateModulo(bytes, width);
    return bytes;
}

static std::string normalizeLiteralToWidth(const std::string& value, int width) {
    return bytesToDecimal(parseIntegerLiteralModulo(value, width));
}

static std::string bitVecLiteral(const std::string& value, int width) {
    if (width <= 0) {
        throw std::runtime_error("SmtEmitter: bit-vector literal width must be known");
    }
    std::ostringstream os;
    os << "(_ bv" << normalizeLiteralToWidth(value, width) << " " << width << ")";
    return os.str();
}

static std::string bitVecLiteral(unsigned long long value, int width) {
    return bitVecLiteral(std::to_string(value), width);
}

static std::string resizeBitVecSmt(std::string value,
                                   int src_width,
                                   int dst_width,
                                   bool signed_value) {
    if (dst_width <= 0) {
        throw std::runtime_error("SmtEmitter: target width must be known");
    }
    if (src_width <= 0 || src_width == dst_width) return value;
    if (src_width > dst_width) {
        return "((_ extract " + std::to_string(dst_width - 1) + " 0) " + value + ")";
    }
    return "((_ " + std::string(signed_value ? "sign_extend" : "zero_extend") + " " +
           std::to_string(dst_width - src_width) + ") " + value + ")";
}

static bool isBoolLiteral(const ExprPtr& e) {
    return e && e->kind == ExprKind::Literal && isBoolType(e->type);
}

static std::string boolLiteralToSmt(const std::string& value) {
    if (value == "true" || value == "1") return "true";
    if (value == "false" || value == "0") return "false";
    return "";
}

static std::string guardToSmt(const ExprPtr& e, const PredicateProgram* prog);
static std::string exprToSmt(const ExprPtr& e, const PredicateProgram* prog);

static bool smtExprYieldsBitVec(const ExprPtr& e) {
    if (!e) return false;
    switch (e->kind) {
    case ExprKind::Literal:
        return !isBoolType(e->type);
    case ExprKind::Slice:
    case ExprKind::WriteSlice:
    case ExprKind::WriteBit:
    case ExprKind::Concat:
    case ExprKind::Repeat:
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc:
        return true;
    case ExprKind::UnaryOp:
        return e->op == "~" || e->op == "-";
    case ExprKind::BinaryOp:
        return e->op == "+" || e->op == "-" || e->op == "*" ||
               e->op == "/" || e->op == "%" ||
               e->op == "&" || e->op == "|" || e->op == "^" ||
               e->op == "<<" || e->op == ">>";
    default:
        return false;
    }
}

static const TypeInfo* declaredVarType(const std::string& name) {
    if (!SmtPrintState::var_types) return nullptr;
    auto it = SmtPrintState::var_types->find(name);
    if (it == SmtPrintState::var_types->end() || it->second.width <= 0) return nullptr;
    return &it->second;
}

static bool signedComparisonFor(const ExprPtr& e) {
    if (!e || !e->left || !e->right) return false;
    bool left_signed = isSignedBitVectorType(e->left->type);
    bool right_signed = isSignedBitVectorType(e->right->type);
    if (left_signed != right_signed) {
        throw std::runtime_error("SmtEmitter: mixed signed/unsigned comparison was not canonicalized");
    }
    return left_signed;
}

static bool signedArithmeticFor(const ExprPtr& e, const std::string& op) {
    if (!e || !e->left || !e->right) return false;
    bool left_signed = isSignedBitVectorType(e->left->type);
    bool right_signed = isSignedBitVectorType(e->right->type);
    if (left_signed != right_signed) {
        throw std::runtime_error("SmtEmitter: mixed signed/unsigned " + op + " was not canonicalized");
    }
    return left_signed;
}

static void rememberVarType(std::unordered_map<std::string, TypeInfo>& var_types,
                            const std::string& name,
                            const TypeInfo& type) {
    if (name.empty() || type.width <= 0) return;
    auto it = var_types.find(name);
    if (it == var_types.end()) {
        var_types[name] = type;
        return;
    }
    if (boolBitVec1Compatible(it->second, type)) {
        if (isBoolType(type)) it->second = type;
        return;
    }
    if (!sameSmtType(it->second, type)) {
        throw std::runtime_error("SmtEmitter: conflicting types for variable '" + name + "'");
    }
}

static TypeInfo typeForVar(const PredicateProgram& prog,
                           const std::string& var,
                           const std::unordered_map<std::string, TypeInfo>& expr_var_types) {
    auto expr_type_it = expr_var_types.find(var);
    if (expr_type_it != expr_var_types.end() && expr_type_it->second.width > 0) {
        return expr_type_it->second;
    }
    auto symbol_it = prog.symbols.find(var);
    if (symbol_it != prog.symbols.end() && symbol_it->second.width > 0) {
        return symbol_it->second;
    }
    for (const auto& out : prog.output_expressions) {
        if (out.name == var && out.type.width > 0) return out.type;
    }

    std::string base = var;
    auto pos = var.rfind('_');
    if (pos != std::string::npos && pos + 1 < var.size()) {
        bool numeric_suffix = true;
        for (size_t i = pos + 1; i < var.size(); ++i) {
            if (var[i] < '0' || var[i] > '9') {
                numeric_suffix = false;
                break;
            }
        }
        if (numeric_suffix) base = var.substr(0, pos);
    }
    auto base_it = prog.symbols.find(base);
    if (base_it != prog.symbols.end() && base_it->second.width > 0) {
        return base_it->second;
    }
    for (const auto& out : prog.output_expressions) {
        if (out.name == base && out.type.width > 0) return out.type;
    }
    auto base_expr_type_it = expr_var_types.find(base);
    if (base_expr_type_it != expr_var_types.end() && base_expr_type_it->second.width > 0) {
        return base_expr_type_it->second;
    }
    throw std::runtime_error("SmtEmitter: type is unknown for variable '" + var + "'");
}

static std::string boolAsBitVec(const ExprPtr& e, const PredicateProgram* prog) {
    if (e && isBoolType(e->type)) {
        return "(ite " + guardToSmt(e, prog) + " #b1 #b0)";
    }
    return resizeBitVecSmt(exprToSmt(e, prog),
                           e ? e->type.width : 0,
                           1,
                           e && isSignedBitVectorType(e->type));
}

static std::string bitVecValueToSmt(const ExprPtr& e,
                                    const PredicateProgram* prog,
                                    int dst_width) {
    if (!e) {
        throw std::runtime_error("SmtEmitter: cannot convert missing expression to BitVec");
    }
    if (dst_width <= 0) {
        throw std::runtime_error("SmtEmitter: target bit-vector width must be known");
    }
    if (isBoolType(e->type)) {
        return resizeBitVecSmt(boolAsBitVec(e, prog), 1, dst_width, false);
    }
    if (e->kind == ExprKind::VarRef) {
        if (const TypeInfo* declared = declaredVarType(e->var_name)) {
            if (isBoolType(*declared)) {
                return resizeBitVecSmt("(ite " + e->var_name + " #b1 #b0)", 1, dst_width, false);
            }
            bool signed_value = e->type.width > 0
                ? isSignedBitVectorType(e->type)
                : isSignedBitVectorType(*declared);
            return resizeBitVecSmt(e->var_name, declared->width, dst_width, signed_value);
        }
    }
    if (e->kind == ExprKind::BitSelect) {
        return resizeBitVecSmt("(ite " + exprToSmt(e, prog) + " #b1 #b0)", 1, dst_width, false);
    }
    return resizeBitVecSmt(exprToSmt(e, prog),
                           e->type.width,
                           dst_width,
                           isSignedBitVectorType(e->type));
}

static std::string valueForTypeToSmt(const ExprPtr& e,
                                     const PredicateProgram* prog,
                                     const TypeInfo& type) {
    if (isBoolType(type)) return guardToSmt(e, prog);
    return bitVecValueToSmt(e, prog, type.width);
}

static std::string bitVecAsBool(const ExprPtr& e, const PredicateProgram* prog) {
    if (!e || e->type.width <= 0) {
        throw std::runtime_error("SmtEmitter: cannot convert unknown-width expression to Bool");
    }
    if (isBoolType(e->type)) return guardToSmt(e, prog);
    if (e->type.width == 1) {
        return "(= " + bitVecValueToSmt(e, prog, 1) + " #b1)";
    }
    return "(not (= " + bitVecValueToSmt(e, prog, e->type.width) + " " +
           bitVecLiteral(0, e->type.width) + "))";
}

static std::string writeSliceToSmt(const ExprPtr& e, const PredicateProgram* prog) {
    int base_width = e && e->base ? e->base->type.width : 0;
    if (base_width <= 0 || e->lo < 0 || e->hi < e->lo || e->hi >= base_width) {
        throw std::runtime_error("SmtEmitter: invalid write_slice range");
    }
    int slice_width = e->hi - e->lo + 1;
    std::string base = bitVecValueToSmt(e->base, prog, base_width);
    std::string value = bitVecValueToSmt(e->value, prog, slice_width);

    if (slice_width == base_width) return value;

    std::vector<std::string> parts;
    if (e->hi + 1 < base_width) {
        parts.push_back("((_ extract " + std::to_string(base_width - 1) + " " +
                        std::to_string(e->hi + 1) + ") " + base + ")");
    }
    parts.push_back(value);
    if (e->lo > 0) {
        parts.push_back("((_ extract " + std::to_string(e->lo - 1) + " 0) " + base + ")");
    }
    if (parts.size() == 1) return parts.front();
    std::string out = "(concat";
    for (const auto& part : parts) out += " " + part;
    return out + ")";
}

static std::string reduceToSmt(const ExprPtr& e, const PredicateProgram* prog) {
    int width = e && e->operand ? e->operand->type.width : 0;
    if (width <= 0) {
        throw std::runtime_error("SmtEmitter: reduce operand width must be known");
    }
    std::string operand = bitVecValueToSmt(e->operand, prog, width);
    if (e->kind == ExprKind::ReduceOr) {
        return "(not (= " + operand + " " + bitVecLiteral(0, width) + "))";
    }
    if (e->kind == ExprKind::ReduceAnd) {
        return "(= " + operand + " (bvnot " + bitVecLiteral(0, width) + "))";
    }
    auto bit_bool = [&](int bit) {
        return "(= ((_ extract " + std::to_string(bit) + " " + std::to_string(bit) +
               ") " + operand + ") #b1)";
    };
    std::string out = bit_bool(0);
    for (int bit = 1; bit < width; ++bit) {
        out = "(xor " + out + " " + bit_bool(bit) + ")";
    }
    return out;
}

static std::string lookupToSmt(const ExprPtr& e, const PredicateProgram* prog) {
    if (!prog || e->args.size() < 2 || !e->args[0] || e->args[0]->kind != ExprKind::Literal) {
        throw std::runtime_error("SmtEmitter: lookup requires serialized table name and index");
    }
    const std::string table_name = e->args[0]->literal_value;
    auto table_it = prog->lookup_tables.find(table_name);
    if (table_it == prog->lookup_tables.end() || table_it->second.empty()) {
        throw std::runtime_error("SmtEmitter: lookup table '" + table_name + "' is not serialized");
    }
    const auto& values = table_it->second;
    const auto& index = e->args[1];
    int index_width = index ? index->type.width : 0;
    int value_width = e->type.width;
    if (index_width <= 0 || value_width <= 0) {
        throw std::runtime_error("SmtEmitter: lookup index/value width must be known");
    }
    std::string index_smt = bitVecValueToSmt(index, prog, index_width);
    bool total = false;
    if (index_width < static_cast<int>(sizeof(size_t) * 8)) {
        total = values.size() == (static_cast<size_t>(1) << index_width);
    }
    if (!total && SmtPrintState::assumptions) {
        SmtPrintState::assumptions->push_back(
            "(bvult " + index_smt + " " +
            bitVecLiteral(static_cast<unsigned long long>(values.size()), index_width) + ")");
    }
    std::string out = bitVecLiteral(0, value_width);
    for (int i = static_cast<int>(values.size()) - 1; i >= 0; --i) {
        std::string cond = "(= " + index_smt + " " + bitVecLiteral(static_cast<unsigned long long>(i), index_width) + ")";
        std::string value = bitVecLiteral(values[static_cast<size_t>(i)], value_width);
        out = "(ite " + cond + " " + value + " " + out + ")";
    }
    return out;
}

static void addDynamicBoundsAssumption(const std::string& index_smt,
                                       int index_width,
                                       int max_index) {
    if (!SmtPrintState::assumptions || index_width <= 0 || max_index < 0) return;
    bool entire_domain_is_valid = false;
    if (index_width < 63) {
        const auto domain_max = (std::uint64_t{1} << index_width) - 1;
        entire_domain_is_valid = domain_max <= static_cast<std::uint64_t>(max_index);
    }
    if (!entire_domain_is_valid) {
        SmtPrintState::assumptions->push_back(
            "(bvule " + index_smt + " " +
            bitVecLiteral(static_cast<unsigned long long>(max_index), index_width) + ")");
    }
}

static std::string dynamicSelectToSmt(const ExprPtr& e,
                                      const PredicateProgram* prog,
                                      bool bit_select) {
    if (!e || e->args.size() < 2 || !e->args[0] || !e->args[1]) {
        throw std::runtime_error("SmtEmitter: dynamic select requires receiver and index");
    }
    const auto& base = e->args[0];
    const auto& index = e->args[1];
    const int base_width = base->type.width;
    const int index_width = index->type.width;
    const int result_width = bit_select ? 1 : e->type.width;
    if (base_width <= 0 || index_width <= 0 || result_width <= 0) {
        throw std::runtime_error("SmtEmitter: dynamic select widths must be known");
    }
    if (result_width > base_width) {
        throw std::runtime_error("SmtEmitter: dynamic range width exceeds receiver width");
    }

    const std::string base_smt = bitVecValueToSmt(base, prog, base_width);
    const std::string index_smt = bitVecValueToSmt(index, prog, index_width);
    const std::string shift_smt =
        resizeBitVecSmt(index_smt, index_width, base_width, false);
    const std::string shifted = "(bvlshr " + base_smt + " " + shift_smt + ")";

    addDynamicBoundsAssumption(index_smt,
                               index_width,
                               base_width - result_width);
    if (bit_select) {
        return "(= ((_ extract 0 0) " + shifted + ") #b1)";
    }
    return "((_ extract " + std::to_string(result_width - 1) +
           " 0) " + shifted + ")";
}

static std::string exprToSmt(const ExprPtr& e, const PredicateProgram* prog) {
    if (!e) return "true";
    SmtPrintFrame frame;
    if (SmtPrintState::depth > 8192 || SmtPrintState::nodes++ > 1000000) {
        throw std::runtime_error("SmtEmitter: expression too large to serialize without elision");
    }

    switch (e->kind) {
    case ExprKind::Literal: {
        if (isBoolType(e->type)) {
            auto bool_value = boolLiteralToSmt(e->literal_value);
            if (!bool_value.empty()) return bool_value;
        }
        if (e->type.width > 0) {
            return bitVecLiteral(e->literal_value, e->type.width);
        }
        if (e->literal_value == "true") return "true";
        if (e->literal_value == "false") return "false";
        throw std::runtime_error("SmtEmitter: unsupported literal '" + e->literal_value + "'");
    }

    case ExprKind::VarRef:
        return e->var_name;

    case ExprKind::BinaryOp: {
        std::string op = e->op;
        if (op == "&&") {
            return "(and " + guardToSmt(e->left, prog) + " " + guardToSmt(e->right, prog) + ")";
        }
        if (op == "||") {
            return "(or " + guardToSmt(e->left, prog) + " " + guardToSmt(e->right, prog) + ")";
        }
        auto operand_width = [&]() {
            int width = e->type.width;
            if (e->left && e->left->type.width > width) width = e->left->type.width;
            if (e->right && e->right->type.width > width) width = e->right->type.width;
            if (width <= 0) throw std::runtime_error("SmtEmitter: binary operand width must be known");
            return width;
        };

        std::string smt_op;
        if (op == "+") smt_op = "bvadd";
        else if (op == "-") smt_op = "bvsub";
        else if (op == "*") smt_op = "bvmul";
        else if (op == "/") smt_op = signedArithmeticFor(e, "division") ? "bvsdiv" : "bvudiv";
        else if (op == "%") smt_op = signedArithmeticFor(e, "modulo") ? "bvsrem" : "bvurem";
        else if (op == "&") smt_op = "bvand";
        else if (op == "|") smt_op = "bvor";
        else if (op == "^") smt_op = "bvxor";
        else if (op == "<<") smt_op = "bvshl";
        else if (op == ">>") smt_op = isSignedBitVectorType(e->left ? e->left->type : TypeInfo{}) ? "bvashr" : "bvlshr";
        else if (op == "==") smt_op = "=";
        else if (op == "!=") smt_op = "=";
        else if (op == "<") smt_op = signedComparisonFor(e) ? "bvslt" : "bvult";
        else if (op == "<=") smt_op = signedComparisonFor(e) ? "bvsle" : "bvule";
        else if (op == ">") smt_op = signedComparisonFor(e) ? "bvsgt" : "bvugt";
        else if (op == ">=") smt_op = signedComparisonFor(e) ? "bvsge" : "bvuge";
        else smt_op = op;

        if (op == "==" || op == "!=") {
            bool both_bool = e->left && e->right && isBoolType(e->left->type) && isBoolType(e->right->type);
            std::string left = both_bool ? guardToSmt(e->left, prog) : bitVecValueToSmt(e->left, prog, operand_width());
            std::string right = both_bool ? guardToSmt(e->right, prog) : bitVecValueToSmt(e->right, prog, operand_width());
            std::string eq = "(= " + left + " " + right + ")";
            return op == "!=" ? "(not " + eq + ")" : eq;
        }

        if (op == "<<" || op == ">>") {
            int left_width = e->left ? e->left->type.width : 0;
            if (left_width <= 0) throw std::runtime_error("SmtEmitter: shift left operand width must be known");
            return "(" + smt_op + " " +
                   bitVecValueToSmt(e->left, prog, left_width) + " " +
                   bitVecValueToSmt(e->right, prog, left_width) + ")";
        }

        int width = operand_width();
        return "(" + smt_op + " " +
               bitVecValueToSmt(e->left, prog, width) + " " +
               bitVecValueToSmt(e->right, prog, width) + ")";
    }

    case ExprKind::UnaryOp: {
        if (e->op == "!") return "(not " + guardToSmt(e->operand, prog) + ")";
        if (e->op == "~") return "(bvnot " + bitVecValueToSmt(e->operand, prog, e->operand ? e->operand->type.width : 0) + ")";
        if (e->op == "-") return "(bvneg " + bitVecValueToSmt(e->operand, prog, e->operand ? e->operand->type.width : 0) + ")";
        return "(" + e->op + " " + exprToSmt(e->operand, prog) + ")";
    }

    case ExprKind::ArrayAccess:
        throw std::runtime_error("SmtEmitter: unlowered ArrayAccess is unsupported");

    case ExprKind::FieldAccess:
        throw std::runtime_error("SmtEmitter: unlowered FieldAccess is unsupported");

    case ExprKind::Call: {
        if (e->intrinsic == IntrinsicKind::DynamicRangeAt || e->intrinsic == IntrinsicKind::DynamicBitAt) {
            return dynamicSelectToSmt(
                e,
                prog,
                e->intrinsic == IntrinsicKind::DynamicBitAt);
        }
        if (e->callee == "lookup") return lookupToSmt(e, prog);
        throw std::runtime_error("SmtEmitter: unsupported remaining Call '" + e->callee + "'");
    }

    case ExprKind::Cast: {
        int w = e->cast_type.width;
        if (w > 0) {
            int src_w = e->cast_expr ? e->cast_expr->type.width : 0;
            if (e->cast_expr && isBoolType(e->cast_expr->type)) {
                if (isBoolType(e->cast_type)) return guardToSmt(e->cast_expr, prog);
                return resizeBitVecSmt(boolAsBitVec(e->cast_expr, prog), 1, w, false);
            }
            if (isBoolType(e->cast_type)) {
                return bitVecAsBool(e->cast_expr, prog);
            }
            if (src_w > 0 && w > src_w) {
                int extra = w - src_w;
                std::string op = isSignedBitVectorType(e->cast_expr->type) ? "sign_extend" : "zero_extend";
                return "((_ " + op + " " + std::to_string(extra) + ") " +
                    bitVecValueToSmt(e->cast_expr, prog, src_w) + ")";
            }
            if (src_w > 0 && w == src_w) {
                return bitVecValueToSmt(e->cast_expr, prog, w);
            }
            return "((_ extract " + std::to_string(w-1) + " 0) " +
                   bitVecValueToSmt(e->cast_expr, prog, src_w) + ")";
        }
        return exprToSmt(e->cast_expr, prog);
    }

    case ExprKind::ZExt: {
        int src_w = e->cast_expr ? e->cast_expr->type.width : 0;
        if (e->cast_expr && isBoolType(e->cast_expr->type)) {
            src_w = 1;
            int extra = e->to_width - src_w;
            if (extra <= 0) return boolAsBitVec(e->cast_expr, prog);
            return "((_ zero_extend " + std::to_string(extra) + ") " + boolAsBitVec(e->cast_expr, prog) + ")";
        }
        int extra = e->to_width - src_w;
        if (extra <= 0) return exprToSmt(e->cast_expr, prog);
        return "((_ zero_extend " + std::to_string(extra) + ") " +
               bitVecValueToSmt(e->cast_expr, prog, src_w) + ")";
    }

    case ExprKind::SExt: {
        int src_w = e->cast_expr ? e->cast_expr->type.width : 0;
        if (e->cast_expr && isBoolType(e->cast_expr->type)) {
            src_w = 1;
            int extra = e->to_width - src_w;
            if (extra <= 0) return boolAsBitVec(e->cast_expr, prog);
            return "((_ zero_extend " + std::to_string(extra) + ") " + boolAsBitVec(e->cast_expr, prog) + ")";
        }
        int extra = e->to_width - src_w;
        if (extra <= 0) return exprToSmt(e->cast_expr, prog);
        return "((_ sign_extend " + std::to_string(extra) + ") " +
               bitVecValueToSmt(e->cast_expr, prog, src_w) + ")";
    }

    case ExprKind::Trunc:
        if (e->cast_expr && isBoolType(e->cast_expr->type)) {
            if (e->to_width != 1) {
                return resizeBitVecSmt(boolAsBitVec(e->cast_expr, prog), 1, e->to_width, false);
            }
            return boolAsBitVec(e->cast_expr, prog);
        }
        return "((_ extract " + std::to_string(e->to_width - 1) + " 0) " +
               bitVecValueToSmt(e->cast_expr, prog, e->cast_expr ? e->cast_expr->type.width : 0) + ")";

    case ExprKind::Slice:
        return "((_ extract " + std::to_string(e->hi) + " " + std::to_string(e->lo) + ") " +
               bitVecValueToSmt(e->base, prog, e->base ? e->base->type.width : 0) + ")";

    case ExprKind::BitSelect:
        return "(= ((_ extract " + std::to_string(e->bit) + " " + std::to_string(e->bit) + ") " +
               bitVecValueToSmt(e->base, prog, e->base ? e->base->type.width : 0) + ") #b1)";

    case ExprKind::Concat: {
        if (e->parts.empty()) {
            throw std::runtime_error("SmtEmitter: concat requires at least one operand");
        }
        if (e->parts.size() == 1) {
            return bitVecValueToSmt(e->parts.front(), prog, e->parts.front() ? e->parts.front()->type.width : 0);
        }
        std::string s = "(concat";
        for (auto& p : e->parts) s += " " + bitVecValueToSmt(p, prog, p ? p->type.width : 0);
        return s + ")";
    }

    case ExprKind::Repeat:
        return "((_ repeat " + std::to_string(e->times) + ") " +
               bitVecValueToSmt(e->operand, prog, e->operand ? e->operand->type.width : 0) + ")";

    case ExprKind::WriteSlice:
        return writeSliceToSmt(e, prog);

    case ExprKind::WriteBit: {
        auto lowered = std::make_shared<Expr>(*e);
        lowered->kind = ExprKind::WriteSlice;
        lowered->hi = e->bit;
        lowered->lo = e->bit;
        return writeSliceToSmt(lowered, prog);
    }

    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor:
        return reduceToSmt(e, prog);

    case ExprKind::Ternary:
        if (isBoolType(e->type)) {
            return "(ite " + guardToSmt(e->cond, prog) + " " +
                   guardToSmt(e->then_expr, prog) + " " +
                   guardToSmt(e->else_expr, prog) + ")";
        }
        return "(ite " + guardToSmt(e->cond, prog) + " " +
               bitVecValueToSmt(e->then_expr, prog, e->type.width) + " " +
               bitVecValueToSmt(e->else_expr, prog, e->type.width) + ")";
    }

    return "?";
}

static std::string guardToSmt(const ExprPtr& e, const PredicateProgram* prog) {
    if (!e) return "true";
    if (isBoolLiteral(e)) {
        auto bool_value = boolLiteralToSmt(e->literal_value);
        if (!bool_value.empty()) return bool_value;
    }
    if (e->kind == ExprKind::VarRef) {
        if (const TypeInfo* declared = declaredVarType(e->var_name)) {
            if (isBoolType(*declared)) return e->var_name;
            return "(not (= " + e->var_name + " " + bitVecLiteral(0, declared->width) + "))";
        }
    }
    if (e->type.width == 1 && smtExprYieldsBitVec(e)) {
        return "(= " + exprToSmt(e, prog) + " #b1)";
    }
    if (isBoolType(e->type)) {
        return exprToSmt(e, prog);
    }
    if (e->kind == ExprKind::BinaryOp) {
        if (e->op == "&&") return "(and " + guardToSmt(e->left, prog) + " " + guardToSmt(e->right, prog) + ")";
        if (e->op == "||") return "(or " + guardToSmt(e->left, prog) + " " + guardToSmt(e->right, prog) + ")";
    }
    if (e->kind == ExprKind::UnaryOp && e->op == "!") {
        return "(not " + guardToSmt(e->operand, prog) + ")";
    }
    return bitVecAsBool(e, prog);
}

// Collect all variable references from an expression
static void collectVars(const ExprPtr& e, std::set<std::string>& vars, int depth = 0, int* nodes = nullptr) {
    if (!e) return;
    int local_nodes = 0;
    if (!nodes) nodes = &local_nodes;
    if (depth > 8192 || (*nodes)++ > 1000000) {
        throw std::runtime_error("SmtEmitter: expression too large to collect variables without elision");
    }
    if (e->kind == ExprKind::VarRef) {
        vars.insert(e->var_name);
        return;
    }
    collectVars(e->left, vars, depth + 1, nodes);
    collectVars(e->right, vars, depth + 1, nodes);
    collectVars(e->operand, vars, depth + 1, nodes);
    collectVars(e->array_base, vars, depth + 1, nodes);
    collectVars(e->index, vars, depth + 1, nodes);
    collectVars(e->struct_base, vars, depth + 1, nodes);
    collectVars(e->cast_expr, vars, depth + 1, nodes);
    collectVars(e->cond, vars, depth + 1, nodes);
    collectVars(e->then_expr, vars, depth + 1, nodes);
    collectVars(e->else_expr, vars, depth + 1, nodes);
    collectVars(e->base, vars, depth + 1, nodes);
    collectVars(e->value, vars, depth + 1, nodes);
    for (auto& arg : e->args) collectVars(arg, vars, depth + 1, nodes);
    for (auto& part : e->parts) collectVars(part, vars, depth + 1, nodes);
}

static void collectVarTypes(const ExprPtr& e,
                            std::unordered_map<std::string, TypeInfo>& var_types,
                            int depth = 0,
                            int* nodes = nullptr) {
    if (!e) return;
    int local_nodes = 0;
    if (!nodes) nodes = &local_nodes;
    if (depth > 8192 || (*nodes)++ > 1000000) {
        throw std::runtime_error("SmtEmitter: expression too large to collect variable types without elision");
    }
    if (e->kind == ExprKind::VarRef) {
        rememberVarType(var_types, e->var_name, e->type);
        return;
    }
    collectVarTypes(e->left, var_types, depth + 1, nodes);
    collectVarTypes(e->right, var_types, depth + 1, nodes);
    collectVarTypes(e->operand, var_types, depth + 1, nodes);
    collectVarTypes(e->array_base, var_types, depth + 1, nodes);
    collectVarTypes(e->index, var_types, depth + 1, nodes);
    collectVarTypes(e->struct_base, var_types, depth + 1, nodes);
    collectVarTypes(e->cast_expr, var_types, depth + 1, nodes);
    collectVarTypes(e->cond, var_types, depth + 1, nodes);
    collectVarTypes(e->then_expr, var_types, depth + 1, nodes);
    collectVarTypes(e->else_expr, var_types, depth + 1, nodes);
    collectVarTypes(e->base, var_types, depth + 1, nodes);
    collectVarTypes(e->value, var_types, depth + 1, nodes);
    for (auto& arg : e->args) collectVarTypes(arg, var_types, depth + 1, nodes);
    for (auto& part : e->parts) collectVarTypes(part, var_types, depth + 1, nodes);
}

static std::string smtTargetName(const ExprPtr& e) {
    if (!e) return "";
    if (e->kind == ExprKind::VarRef) return e->var_name;
    return "";
}

static std::string stripSsaSuffix(const std::string& var) {
    auto pos = var.rfind('_');
    if (pos == std::string::npos || pos + 1 >= var.size()) return var;
    for (size_t i = pos + 1; i < var.size(); ++i) {
        if (var[i] < '0' || var[i] > '9') return var;
    }
    return var.substr(0, pos);
}

static std::string previousSsaVersion(const std::string& var) {
    auto pos = var.rfind('_');
    if (pos == std::string::npos || pos + 1 >= var.size()) return {};
    for (size_t i = pos + 1; i < var.size(); ++i) {
        if (var[i] < '0' || var[i] > '9') return {};
    }
    int version = 0;
    try {
        version = std::stoi(var.substr(pos + 1));
    } catch (...) {
        return {};
    }
    if (version <= 0) return {};
    return var.substr(0, pos + 1) + std::to_string(version - 1);
}

static bool isUnconditionalGuard(const ExprPtr& guard) {
    return !guard ||
           (guard->kind == ExprKind::Literal &&
            (guard->literal_value == "1" || guard->literal_value == "true"));
}

static bool isExternalSmtLeaf(const PredicateProgram& prog, const std::string& var) {
    auto is_input_direction = [&](const std::string& name) {
        auto it = prog.param_directions.find(name);
        return it != prog.param_directions.end() &&
               (it->second == "Input" || it->second == "InOut");
    };
    auto has_input_prefix_direction = [&](const std::string& name) {
        size_t best = 0;
        bool matched = false;
        for (const auto& item : prog.param_directions) {
            const std::string prefix = item.first + "_";
            if (name.rfind(prefix, 0) == 0 && item.first.size() > best) {
                best = item.first.size();
                matched = item.second == "Input" || item.second == "InOut";
            }
        }
        return matched;
    };
    std::string current = var;
    while (!current.empty()) {
        for (const auto& input : prog.inputs) {
            if (current == input) return true;
        }
        if (is_input_direction(current) || has_input_prefix_direction(current)) return true;
        std::string parent = stripSsaSuffix(current);
        if (parent == current) break;
        current = parent;
    }
    return false;
}

} // namespace

std::string emitSmt(const PredicateProgram& prog) {
    std::ostringstream os;

    os << "; predicate expansion for function: " << prog.function_name << "\n";
    os << "; generated by predicate-expand\n";
    for (const auto& [name, direction] : prog.param_directions) {
        os << "; param_direction " << name << " " << direction << "\n";
    }
    os << "\n";

    std::vector<bool> include_assignment(prog.assignments.size(), false);
    std::set<std::string> all_assignment_targets;
    for (const auto& assignment : prog.assignments) {
        std::string target = smtTargetName(assignment.target);
        if (!target.empty()) all_assignment_targets.insert(target);
    }
    std::set<std::string> required_vars;
    for (const auto& out : prog.output_expressions) {
        required_vars.insert(out.name);
        collectVars(out.expr, required_vars);
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = static_cast<int>(prog.assignments.size()) - 1; i >= 0; --i) {
            if (include_assignment[static_cast<size_t>(i)]) continue;
            const auto& ga = prog.assignments[static_cast<size_t>(i)];
            std::string target = smtTargetName(ga.target);
            if (target.empty() || required_vars.count(target) == 0) continue;
            include_assignment[static_cast<size_t>(i)] = true;
            std::set<std::string> deps;
            collectVars(ga.guard, deps);
            collectVars(ga.value, deps);
            if (!isUnconditionalGuard(ga.guard)) {
                std::string previous = previousSsaVersion(target);
                if (!previous.empty() &&
                    (all_assignment_targets.count(previous) != 0 ||
                     isExternalSmtLeaf(prog, previous))) {
                    deps.insert(previous);
                }
            }
            for (const auto& dep : deps) {
                if (required_vars.insert(dep).second) changed = true;
            }
            changed = true;
        }
    }

    std::set<std::string> kept_targets;
    for (size_t i = 0; i < prog.assignments.size(); ++i) {
        if (!include_assignment[i]) continue;
        std::string target = smtTargetName(prog.assignments[i].target);
        if (!target.empty()) kept_targets.insert(target);
    }
    std::set<std::string> output_names;
    for (const auto& out : prog.output_expressions) {
        output_names.insert(out.name);
    }
    for (const auto& var : required_vars) {
        if (kept_targets.count(var) != 0 || output_names.count(var) != 0 ||
            isExternalSmtLeaf(prog, var)) {
            continue;
        }
        throw std::runtime_error("SmtEmitter: required internal variable '" + var +
                                 "' has no defining assignment");
    }

    // Declare variables
    std::set<std::string> all_vars;
    std::unordered_map<std::string, TypeInfo> expr_var_types;
    std::unordered_map<std::string, TypeInfo> declared_var_types;
    for (size_t i = 0; i < prog.assignments.size(); ++i) {
        if (!include_assignment[i]) continue;
        const auto& ga = prog.assignments[i];
        collectVars(ga.guard, all_vars);
        collectVars(ga.target, all_vars);
        collectVars(ga.value, all_vars);
        collectVarTypes(ga.guard, expr_var_types);
        collectVarTypes(ga.target, expr_var_types);
        collectVarTypes(ga.value, expr_var_types);
        if (!isUnconditionalGuard(ga.guard)) {
            std::string previous = previousSsaVersion(smtTargetName(ga.target));
            if (!previous.empty() && required_vars.count(previous) != 0) {
                all_vars.insert(previous);
                TypeInfo target_type =
                    ga.type.width > 0 ? ga.type : (ga.target ? ga.target->type : TypeInfo{});
                rememberVarType(expr_var_types, previous, target_type);
            }
        }
    }
    for (auto& out : prog.output_expressions) {
        all_vars.insert(out.name);
        rememberVarType(expr_var_types, out.name, out.type);
        collectVars(out.expr, all_vars);
        collectVarTypes(out.expr, expr_var_types);
    }

    for (auto& var : all_vars) {
        TypeInfo type = typeForVar(prog, var, expr_var_types);
        rememberVarType(declared_var_types, var, type);
        int width = type.width > 0 ? type.width : 32;
        if (isBoolType(type)) {
            os << "(declare-const " << var << " Bool)\n";
        } else {
            os << "(declare-const " << var << " (_ BitVec " << width << "))\n";
        }
    }
    os << "\n";

    std::vector<std::string> assumptions;
    std::ostringstream body;
    {
        SmtAssumptionScope assumption_scope(assumptions);
        SmtVarTypeScope var_type_scope(declared_var_types);

        // Emit assignments as assertions
        for (size_t i = 0; i < prog.assignments.size(); ++i) {
            if (!include_assignment[i]) continue;
            const auto& ga = prog.assignments[i];
            std::string target = exprToSmt(ga.target, &prog);
            TypeInfo target_type = ga.type.width > 0 ? ga.type : (ga.target ? ga.target->type : TypeInfo{});
            std::string value = valueForTypeToSmt(ga.value, &prog, target_type);
            std::string guard = guardToSmt(ga.guard, &prog);

            if (guard == "true" || guard == "1") {
                body << "(assert (= " << target << " " << value << "))\n";
            } else {
                std::string previous = previousSsaVersion(smtTargetName(ga.target));
                if (!previous.empty() && declared_var_types.count(previous) != 0) {
                    std::string fallback = valueForTypeToSmt(
                        make_var(previous, declared_var_types.at(previous)),
                        &prog,
                        target_type);
                    body << "(assert (= " << target << " (ite " << guard << " "
                         << value << " " << fallback << ")))\n";
                } else {
                    body << "(assert (=> " << guard << " (= " << target << " "
                         << value << ")))\n";
                }
            }
        }

        body << "\n; output_expressions\n";
        for (auto& out : prog.output_expressions) {
            std::string value = valueForTypeToSmt(out.expr, &prog, out.type);
            body << "(assert (= " << out.name << " " << value << "))\n";
        }
    }

    std::set<std::string> unique_assumptions(assumptions.begin(), assumptions.end());
    if (!unique_assumptions.empty()) {
        os << "; lookup/domain assumptions\n";
        for (const auto& assumption : unique_assumptions) {
            os << "(assert " << assumption << ")\n";
        }
        os << "\n";
    }
    os << body.str();

    os << "\n(check-sat)\n";
    return os.str();
}

} // namespace pred
