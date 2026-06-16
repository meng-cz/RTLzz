#include "emitter/SmtEmitter.h"
#include <algorithm>
#include <cctype>
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
};

thread_local int SmtPrintState::depth = 0;
thread_local int SmtPrintState::nodes = 0;

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

static bool isBoolType(const TypeInfo& type) {
    return type.hw_kind == "bool" || type.name == "bool";
}

static bool isSignedBitVectorType(const TypeInfo& type) {
    return !isBoolType(type) && (type.is_signed || type.hw_kind == "signed_view");
}

static bool sameSmtType(const TypeInfo& a, const TypeInfo& b) {
    return a.width == b.width && isBoolType(a) == isBoolType(b) &&
           isSignedBitVectorType(a) == isSignedBitVectorType(b);
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

static bool signedComparisonFor(const ExprPtr& e) {
    if (!e || !e->left || !e->right) return false;
    bool left_signed = isSignedBitVectorType(e->left->type);
    bool right_signed = isSignedBitVectorType(e->right->type);
    if (left_signed != right_signed) {
        throw std::runtime_error("SmtEmitter: mixed signed/unsigned comparison was not canonicalized");
    }
    if (e->left->type.width > 0 && e->right->type.width > 0 &&
        e->left->type.width != e->right->type.width) {
        throw std::runtime_error("SmtEmitter: comparison operands have mismatched widths");
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

static std::string bitVecAsBool(const ExprPtr& e, const PredicateProgram* prog) {
    if (!e || e->type.width <= 0) {
        throw std::runtime_error("SmtEmitter: cannot convert unknown-width expression to Bool");
    }
    return "(not (= " + exprToSmt(e, prog) + " " + bitVecLiteral(0, e->type.width) + "))";
}

static std::string writeSliceToSmt(const ExprPtr& e, const PredicateProgram* prog) {
    int base_width = e && e->base ? e->base->type.width : 0;
    if (base_width <= 0 || e->lo < 0 || e->hi < e->lo || e->hi >= base_width) {
        throw std::runtime_error("SmtEmitter: invalid write_slice range");
    }
    int slice_width = e->hi - e->lo + 1;
    std::string base = exprToSmt(e->base, prog);
    std::string value = e->value && isBoolType(e->value->type)
        ? resizeBitVecSmt(boolAsBitVec(e->value, prog), 1, slice_width, false)
        : resizeBitVecSmt(exprToSmt(e->value, prog),
                           e->value ? e->value->type.width : 0,
                           slice_width,
                           e->value && isSignedBitVectorType(e->value->type));

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
    std::string operand = exprToSmt(e->operand, prog);
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
    std::string index_smt = exprToSmt(index, prog);
    bool table_is_total = false;
    if (index_width < static_cast<int>(sizeof(size_t) * 8)) {
        table_is_total = values.size() == (size_t{1} << index_width);
    }
    if (!table_is_total) {
        throw std::runtime_error("SmtEmitter: lookup table '" + table_name +
            "' is not total for index width " + std::to_string(index_width));
    }

    std::string out = bitVecLiteral(values.back(), value_width);
    for (int i = static_cast<int>(values.size()) - 2; i >= 0; --i) {
        std::string cond = "(= " + index_smt + " " + bitVecLiteral(static_cast<unsigned long long>(i), index_width) + ")";
        std::string value = bitVecLiteral(values[static_cast<size_t>(i)], value_width);
        out = "(ite " + cond + " " + value + " " + out + ")";
    }
    return out;
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
        std::string smt_op;
        if (op == "+") smt_op = "bvadd";
        else if (op == "-") smt_op = "bvsub";
        else if (op == "*") smt_op = "bvmul";
        else if (op == "/" || op == "%") {
            throw std::runtime_error("SmtEmitter: division/modulo is unsupported in Predicate IR SMT");
        }
        else if (op == "&") smt_op = "bvand";
        else if (op == "|") smt_op = "bvor";
        else if (op == "^") smt_op = "bvxor";
        else if (op == "<<") smt_op = "bvshl";
        else if (op == ">>") smt_op = isSignedBitVectorType(e->left ? e->left->type : TypeInfo{}) ? "bvashr" : "bvlshr";
        else if (op == "==") smt_op = "=";
        else if (op == "!=") return "(not (= " + exprToSmt(e->left, prog) + " " + exprToSmt(e->right, prog) + "))";
        else if (op == "<") smt_op = signedComparisonFor(e) ? "bvslt" : "bvult";
        else if (op == "<=") smt_op = signedComparisonFor(e) ? "bvsle" : "bvule";
        else if (op == ">") smt_op = signedComparisonFor(e) ? "bvsgt" : "bvugt";
        else if (op == ">=") smt_op = signedComparisonFor(e) ? "bvsge" : "bvuge";
        else smt_op = op;

        return "(" + smt_op + " " + exprToSmt(e->left, prog) + " " + exprToSmt(e->right, prog) + ")";
    }

    case ExprKind::UnaryOp: {
        if (e->op == "!") return "(not " + guardToSmt(e->operand, prog) + ")";
        if (e->op == "~") return "(bvnot " + exprToSmt(e->operand, prog) + ")";
        if (e->op == "-") return "(bvneg " + exprToSmt(e->operand, prog) + ")";
        return "(" + e->op + " " + exprToSmt(e->operand, prog) + ")";
    }

    case ExprKind::ArrayAccess:
        throw std::runtime_error("SmtEmitter: unlowered ArrayAccess is unsupported");

    case ExprKind::FieldAccess:
        throw std::runtime_error("SmtEmitter: unlowered FieldAccess is unsupported");

    case ExprKind::Call: {
        if (e->intrinsic == IntrinsicKind::DynamicRangeAt || e->intrinsic == IntrinsicKind::DynamicBitAt) {
            throw std::runtime_error(e->intrinsic == IntrinsicKind::DynamicBitAt
                ? "SmtEmitter: dynamic bit select is not supported yet"
                : "SmtEmitter: dynamic range select is not supported yet");
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
                    exprToSmt(e->cast_expr, prog) + ")";
            }
            if (src_w > 0 && w == src_w) {
                return exprToSmt(e->cast_expr, prog);
            }
            return "((_ extract " + std::to_string(w-1) + " 0) " + exprToSmt(e->cast_expr, prog) + ")";
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
        return "((_ zero_extend " + std::to_string(extra) + ") " + exprToSmt(e->cast_expr, prog) + ")";
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
        return "((_ sign_extend " + std::to_string(extra) + ") " + exprToSmt(e->cast_expr, prog) + ")";
    }

    case ExprKind::Trunc:
        if (e->cast_expr && isBoolType(e->cast_expr->type)) {
            if (e->to_width != 1) {
                return resizeBitVecSmt(boolAsBitVec(e->cast_expr, prog), 1, e->to_width, false);
            }
            return boolAsBitVec(e->cast_expr, prog);
        }
        return "((_ extract " + std::to_string(e->to_width - 1) + " 0) " + exprToSmt(e->cast_expr, prog) + ")";

    case ExprKind::Slice:
        return "((_ extract " + std::to_string(e->hi) + " " + std::to_string(e->lo) + ") " +
               exprToSmt(e->base, prog) + ")";

    case ExprKind::BitSelect:
        return "(= ((_ extract " + std::to_string(e->bit) + " " + std::to_string(e->bit) + ") " +
               exprToSmt(e->base, prog) + ") #b1)";

    case ExprKind::Concat: {
        std::string s = "(concat";
        for (auto& p : e->parts) s += " " + exprToSmt(p, prog);
        return s + ")";
    }

    case ExprKind::Repeat:
        return "((_ repeat " + std::to_string(e->times) + ") " + exprToSmt(e->operand, prog) + ")";

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
        return "(ite " + guardToSmt(e->cond, prog) + " " +
               exprToSmt(e->then_expr, prog) + " " +
               exprToSmt(e->else_expr, prog) + ")";
    }

    return "?";
}

static std::string guardToSmt(const ExprPtr& e, const PredicateProgram* prog) {
    if (!e) return "true";
    if (isBoolLiteral(e)) {
        auto bool_value = boolLiteralToSmt(e->literal_value);
        if (!bool_value.empty()) return bool_value;
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
    for (size_t i = 0; i < prog.assignments.size(); ++i) {
        if (!include_assignment[i]) continue;
        const auto& ga = prog.assignments[i];
        collectVars(ga.guard, all_vars);
        collectVars(ga.target, all_vars);
        collectVars(ga.value, all_vars);
        collectVarTypes(ga.guard, expr_var_types);
        collectVarTypes(ga.target, expr_var_types);
        collectVarTypes(ga.value, expr_var_types);
    }
    for (auto& out : prog.output_expressions) {
        all_vars.insert(out.name);
        rememberVarType(expr_var_types, out.name, out.type);
        collectVars(out.expr, all_vars);
        collectVarTypes(out.expr, expr_var_types);
    }

    for (auto& var : all_vars) {
        TypeInfo type = typeForVar(prog, var, expr_var_types);
        int width = type.width > 0 ? type.width : 32;
        if (isBoolType(type)) {
            os << "(declare-const " << var << " Bool)\n";
        } else {
            os << "(declare-const " << var << " (_ BitVec " << width << "))\n";
        }
    }
    os << "\n";

    // Emit assignments as assertions
    for (size_t i = 0; i < prog.assignments.size(); ++i) {
        if (!include_assignment[i]) continue;
        const auto& ga = prog.assignments[i];
        std::string target = exprToSmt(ga.target, &prog);
        TypeInfo target_type = ga.type.width > 0 ? ga.type : (ga.target ? ga.target->type : TypeInfo{});
        std::string value = isBoolType(target_type) ? guardToSmt(ga.value, &prog) : exprToSmt(ga.value, &prog);
        std::string guard = guardToSmt(ga.guard, &prog);

        if (guard == "true" || guard == "1") {
            os << "(assert (= " << target << " " << value << "))\n";
        } else {
            os << "(assert (=> " << guard << " (= " << target << " " << value << ")))\n";
        }
    }

    os << "\n; output_expressions\n";
    for (auto& out : prog.output_expressions) {
        std::string value = isBoolType(out.type) ? guardToSmt(out.expr, &prog) : exprToSmt(out.expr, &prog);
        os << "(assert (= " << out.name << " " << value << "))\n";
    }

    os << "\n(check-sat)\n";
    return os.str();
}

} // namespace pred
