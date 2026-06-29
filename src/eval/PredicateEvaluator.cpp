#include "eval/PredicateEvaluator.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace pred {
namespace {

int limbCount(int width) {
    return width <= 0 ? 0 : (width + 63) / 64;
}

int literalWidth(const ExprPtr& e) {
    return e && e->type.width > 0 ? e->type.width : 64;
}

} // namespace

std::string EvalBits::hex() const {
    if (width <= 0) return "0x0";
    std::ostringstream os;
    os << "0x";
    bool emitted = false;
    for (int i = static_cast<int>(limbs.size()) - 1; i >= 0; --i) {
        if (!emitted) {
            if (limbs[static_cast<size_t>(i)] == 0 && i != 0) continue;
            os << std::hex << limbs[static_cast<size_t>(i)];
            emitted = true;
        } else {
            os.width(16);
            os.fill('0');
            os << std::hex << limbs[static_cast<size_t>(i)];
        }
    }
    if (!emitted) os << "0";
    return os.str();
}

void PredicateEvaluator::setVar(const std::string& name, EvalBits value) {
    vars_[name] = mask(std::move(value));
}

void PredicateEvaluator::setLookupTable(const std::string& name, std::vector<EvalBits> values) {
    for (auto& value : values) value = mask(std::move(value));
    lookup_tables_[name] = std::move(values);
}

EvalBits PredicateEvaluator::fromUInt64(std::uint64_t value, int width, bool is_signed) {
    EvalBits out;
    out.width = width;
    out.is_signed = is_signed;
    out.limbs.assign(static_cast<size_t>(limbCount(width)), 0);
    if (!out.limbs.empty()) out.limbs[0] = value;
    return mask(std::move(out));
}

EvalBits PredicateEvaluator::fromLiteral(const std::string& text, int width, bool is_signed) {
    EvalBits out = fromUInt64(0, width, is_signed);
    std::string s = text;
    int base = 10;
    size_t pos = 0;
    if (s == "true") return fromUInt64(1, width, is_signed);
    if (s == "false") return fromUInt64(0, width, is_signed);
    bool negative = false;
    if (!s.empty() && s.front() == '-') {
        negative = true;
        pos = 1;
    }
    if (s.rfind("0x", pos) == pos || s.rfind("0X", pos) == pos) {
        base = 16;
        pos += 2;
    }
    for (; pos < s.size(); ++pos) {
        char c = s[pos];
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') digit = 10 + c - 'A';
        else continue;
        if (digit < 0 || digit >= base) throw std::runtime_error("bad literal");
        out = base == 16 ? shl(out, 4, width) : add(shl(out, 3, width), shl(out, 1, width), width);
        out = add(out, fromUInt64(static_cast<std::uint64_t>(digit), width), width);
    }
    if (negative) {
        for (auto& limb : out.limbs) limb = ~limb;
        out = add(mask(std::move(out)), fromUInt64(1, width, is_signed), width);
    }
    return mask(std::move(out));
}

EvalBits PredicateEvaluator::eval(const ExprPtr& expr) const {
    if (!expr) throw std::runtime_error("null expression");
    switch (expr->kind) {
    case ExprKind::Literal:
        return fromLiteral(expr->literal_value, literalWidth(expr), expr->type.is_signed);
    case ExprKind::VarRef: {
        auto it = vars_.find(expr->var_name);
        if (it == vars_.end()) throw std::runtime_error("unknown evaluator var: " + expr->var_name);
        auto out = it->second;
        if (expr->type.width > 0 && out.width != expr->type.width) {
            out.width = expr->type.width;
            out.limbs.resize(static_cast<size_t>(limbCount(out.width)), 0);
            out = mask(std::move(out));
        }
        // A VarRef may be a same-width canonical reinterpretation inserted by
        // C++ usual arithmetic conversions. The expression type, not the
        // storage seed's original signedness, determines comparison/shift
        // semantics at this use site.
        out.is_signed = signedType(expr->type);
        return out;
    }
    case ExprKind::BinaryOp: {
        auto a = eval(expr->left);
        auto b = eval(expr->right);
        int width = literalWidth(expr);
        if (expr->op == "+") return add(a, b, width);
        if (expr->op == "-") return sub(a, b, width);
        if (expr->op == "*") return mul(a, b, width, a.is_signed || b.is_signed || signedType(expr->type));
        if (expr->op == "&") return bitwise(a, b, width, '&');
        if (expr->op == "|") return bitwise(a, b, width, '|');
        if (expr->op == "^") return bitwise(a, b, width, '^');
        if (expr->op == "&&") return fromUInt64((compareUnsigned(a, fromUInt64(0, a.width)) != 0 &&
                                                  compareUnsigned(b, fromUInt64(0, b.width)) != 0) ? 1 : 0, 1);
        if (expr->op == "||") return fromUInt64((compareUnsigned(a, fromUInt64(0, a.width)) != 0 ||
                                                  compareUnsigned(b, fromUInt64(0, b.width)) != 0) ? 1 : 0, 1);
        if (expr->op == "<<") return shl(a, shiftAmount(b), width);
        if (expr->op == ">>") {
            if (a.is_signed || signedType(expr->left ? expr->left->type : TypeInfo{})) return ashr(a, shiftAmount(b), width);
            return lshr(a, shiftAmount(b), width);
        }
        if (expr->op == "==" || expr->op == "!=" || expr->op == "<" || expr->op == "<=" ||
            expr->op == ">" || expr->op == ">=") {
            bool signed_semantics = a.is_signed || b.is_signed ||
                signedType(expr->left ? expr->left->type : TypeInfo{}) ||
                signedType(expr->right ? expr->right->type : TypeInfo{});
            int cmp = signed_semantics ? compareSigned(a, b) : compareUnsigned(a, b);
            bool result = false;
            if (expr->op == "==") result = cmp == 0;
            else if (expr->op == "!=") result = cmp != 0;
            else if (expr->op == "<") result = cmp < 0;
            else if (expr->op == "<=") result = cmp <= 0;
            else if (expr->op == ">") result = cmp > 0;
            else if (expr->op == ">=") result = cmp >= 0;
            return fromUInt64(result ? 1 : 0, 1);
        }
        throw std::runtime_error("unsupported evaluator binary op: " + expr->op);
    }
    case ExprKind::UnaryOp: {
        auto v = eval(expr->operand);
        int width = literalWidth(expr);
        if (expr->op == "!") return fromUInt64(compareUnsigned(v, fromUInt64(0, v.width)) == 0 ? 1 : 0, 1);
        if (expr->op == "~") {
            EvalBits out = v;
            out.width = width;
            out.limbs.resize(static_cast<size_t>(limbCount(width)), 0);
            for (auto& limb : out.limbs) limb = ~limb;
            return mask(std::move(out));
        }
        if (expr->op == "-") {
            v.width = width;
            v.limbs.resize(static_cast<size_t>(limbCount(width)), 0);
            return twosComplement(mask(std::move(v)));
        }
        throw std::runtime_error("unsupported evaluator unary op: " + expr->op);
    }
    case ExprKind::Ternary: {
        auto c = eval(expr->cond);
        return eval(compareUnsigned(c, fromUInt64(0, c.width)) != 0 ? expr->then_expr : expr->else_expr);
    }
    case ExprKind::Cast: {
        auto v = eval(expr->cast_expr);
        if (expr->cast_type.width > 0) {
            v.width = expr->cast_type.width;
            v.limbs.resize(static_cast<size_t>(limbCount(v.width)), 0);
        }
        v.is_signed = signedType(expr->cast_type);
        return mask(std::move(v));
    }
    case ExprKind::ZExt:
    case ExprKind::Trunc: {
        auto v = eval(expr->cast_expr);
        v.width = expr->to_width;
        v.is_signed = signedType(expr->type);
        v.limbs.resize(static_cast<size_t>(limbCount(v.width)), 0);
        return mask(std::move(v));
    }
    case ExprKind::SExt:
        return signExtend(eval(expr->cast_expr), expr->to_width);
    case ExprKind::Slice:
        return slice(eval(expr->base), expr->hi, expr->lo);
    case ExprKind::BitSelect:
        return fromUInt64(bit(eval(expr->base), expr->bit) ? 1 : 0, 1);
    case ExprKind::WriteSlice:
        return writeSlice(eval(expr->base), expr->hi, expr->lo, eval(expr->value));
    case ExprKind::WriteBit:
        return writeBit(eval(expr->base), expr->bit, eval(expr->value));
    case ExprKind::DynamicWriteSlice: {
        auto base = eval(expr->base);
        int lo = shiftAmount(eval(expr->index));
        auto value = eval(expr->value);
        int width = value.width > 0 ? value.width : expr->value->type.width;
        return writeSlice(std::move(base), lo + width - 1, lo, std::move(value));
    }
    case ExprKind::DynamicWriteBit: {
        auto base = eval(expr->base);
        int bit_index = shiftAmount(eval(expr->index));
        return writeBit(std::move(base), bit_index, eval(expr->value));
    }
    case ExprKind::Concat: {
        std::vector<EvalBits> parts;
        for (const auto& part : expr->parts) parts.push_back(eval(part));
        return concat(parts);
    }
    case ExprKind::Repeat: {
        std::vector<EvalBits> parts;
        auto v = eval(expr->operand);
        for (int i = 0; i < expr->times; ++i) parts.push_back(v);
        return concat(parts);
    }
    case ExprKind::ReduceOr: {
        auto v = eval(expr->operand);
        return fromUInt64(compareUnsigned(v, fromUInt64(0, v.width)) != 0 ? 1 : 0, 1);
    }
    case ExprKind::ReduceAnd: {
        auto v = eval(expr->operand);
        bool all = v.width > 0;
        for (int i = 0; i < v.width; ++i) all = all && bit(v, i);
        return fromUInt64(all ? 1 : 0, 1);
    }
    case ExprKind::ReduceXor: {
        auto v = eval(expr->operand);
        bool parity = false;
        for (int i = 0; i < v.width; ++i) parity = parity != bit(v, i);
        return fromUInt64(parity ? 1 : 0, 1);
    }
    case ExprKind::Call: {
        if (expr->callee == "lookup") {
            if (expr->args.size() < 2 || !expr->args[0] || expr->args[0]->kind != ExprKind::Literal) {
                throw std::runtime_error("lookup evaluator requires literal table name");
            }
            auto table = lookup_tables_.find(expr->args[0]->literal_value);
            if (table == lookup_tables_.end()) {
                throw std::runtime_error("unknown evaluator lookup table: " + expr->args[0]->literal_value);
            }
            int index = shiftAmount(eval(expr->args[1]));
            if (index < 0 || index >= static_cast<int>(table->second.size())) {
                throw std::runtime_error("lookup evaluator index out of range");
            }
            auto out = table->second[static_cast<size_t>(index)];
            if (expr->type.width > 0 && out.width != expr->type.width) {
                out.width = expr->type.width;
                out.limbs.resize(static_cast<size_t>(limbCount(out.width)), 0);
                out = mask(std::move(out));
            }
            return out;
        }
        if (expr->intrinsic == IntrinsicKind::DynamicRangeAt || expr->callee == "__dynamic_range_at") {
            if (expr->args.size() < 2) throw std::runtime_error("dynamic_range_at evaluator requires receiver and index");
            auto base = eval(expr->args[0]);
            int lo = shiftAmount(eval(expr->args[1]));
            int width = literalWidth(expr);
            return slice(base, lo + width - 1, lo);
        }
        if (expr->intrinsic == IntrinsicKind::DynamicBitAt || expr->callee == "__dynamic_bit_at") {
            if (expr->args.size() < 2) throw std::runtime_error("dynamic_bit_at evaluator requires receiver and index");
            auto base = eval(expr->args[0]);
            int index = shiftAmount(eval(expr->args[1]));
            return fromUInt64(bit(base, index) ? 1 : 0, 1);
        }
        throw std::runtime_error("unsupported evaluator call: " + expr->callee);
    }
    default:
        throw std::runtime_error("unsupported evaluator expression kind");
    }
}

EvalBits PredicateEvaluator::mask(EvalBits value) {
    value.limbs.resize(static_cast<size_t>(limbCount(value.width)), 0);
    if (!value.limbs.empty() && value.width % 64 != 0) {
        value.limbs.back() &= ((std::uint64_t{1} << (value.width % 64)) - 1);
    }
    return value;
}

bool PredicateEvaluator::bit(const EvalBits& value, int index) {
    if (index < 0 || index >= value.width) return false;
    size_t limb = static_cast<size_t>(index / 64);
    int off = index % 64;
    return limb < value.limbs.size() && ((value.limbs[limb] >> off) & 1U) != 0;
}

void PredicateEvaluator::setBit(EvalBits& value, int index, bool bit_value) {
    if (index < 0 || index >= value.width) return;
    size_t limb = static_cast<size_t>(index / 64);
    int off = index % 64;
    if (limb >= value.limbs.size()) value.limbs.resize(limb + 1, 0);
    if (bit_value) value.limbs[limb] |= (std::uint64_t{1} << off);
    else value.limbs[limb] &= ~(std::uint64_t{1} << off);
}

EvalBits PredicateEvaluator::add(const EvalBits& a, const EvalBits& b, int width) {
    EvalBits out = fromUInt64(0, width);
    unsigned carry = 0;
    for (size_t i = 0; i < out.limbs.size(); ++i) {
        std::uint64_t av = i < a.limbs.size() ? a.limbs[i] : 0;
        std::uint64_t bv = i < b.limbs.size() ? b.limbs[i] : 0;
        std::uint64_t sum = av + bv;
        unsigned c1 = sum < av ? 1 : 0;
        std::uint64_t sum2 = sum + carry;
        unsigned c2 = sum2 < sum ? 1 : 0;
        out.limbs[i] = sum2;
        carry = c1 | c2;
    }
    return mask(std::move(out));
}

EvalBits PredicateEvaluator::sub(const EvalBits& a, const EvalBits& b, int width) {
    EvalBits lhs = a;
    lhs.width = width;
    lhs.limbs.resize(static_cast<size_t>(limbCount(width)), 0);
    EvalBits rhs = b;
    rhs.width = width;
    rhs.limbs.resize(static_cast<size_t>(limbCount(width)), 0);
    return add(mask(std::move(lhs)), twosComplement(mask(std::move(rhs))), width);
}

EvalBits PredicateEvaluator::mul(const EvalBits& a, const EvalBits& b, int width, bool signed_semantics) {
    EvalBits lhs = a;
    EvalBits rhs = b;
    bool negate = false;
    if (signed_semantics) {
        if (bit(lhs, lhs.width - 1)) {
            lhs = twosComplement(lhs);
            negate = !negate;
        }
        if (bit(rhs, rhs.width - 1)) {
            rhs = twosComplement(rhs);
            negate = !negate;
        }
    }
    EvalBits out = fromUInt64(0, width);
    for (int i = 0; i < rhs.width; ++i) {
        if (bit(rhs, i)) out = add(out, shl(lhs, i, width), width);
    }
    if (negate) out = twosComplement(out);
    out.is_signed = signed_semantics;
    return mask(std::move(out));
}

EvalBits PredicateEvaluator::bitwise(const EvalBits& a, const EvalBits& b, int width, char op) {
    EvalBits out = fromUInt64(0, width);
    for (size_t i = 0; i < out.limbs.size(); ++i) {
        std::uint64_t av = i < a.limbs.size() ? a.limbs[i] : 0;
        std::uint64_t bv = i < b.limbs.size() ? b.limbs[i] : 0;
        if (op == '&') out.limbs[i] = av & bv;
        if (op == '|') out.limbs[i] = av | bv;
        if (op == '^') out.limbs[i] = av ^ bv;
    }
    return mask(std::move(out));
}

EvalBits PredicateEvaluator::shl(const EvalBits& a, int amount, int width) {
    EvalBits out = fromUInt64(0, width);
    if (amount < 0 || amount >= width) return out;
    for (int i = 0; i < a.width; ++i) {
        if (bit(a, i)) setBit(out, i + amount, true);
    }
    return mask(std::move(out));
}

EvalBits PredicateEvaluator::lshr(const EvalBits& a, int amount, int width) {
    EvalBits out = fromUInt64(0, width);
    if (amount < 0 || amount >= width) return out;
    for (int i = amount; i < a.width; ++i) {
        if (bit(a, i)) setBit(out, i - amount, true);
    }
    return mask(std::move(out));
}

EvalBits PredicateEvaluator::ashr(const EvalBits& a, int amount, int width) {
    EvalBits out = fromUInt64(0, width, true);
    bool sign = bit(a, a.width - 1);
    if (amount < 0) return out;
    if (amount >= width) {
        if (sign) {
            for (int i = 0; i < width; ++i) setBit(out, i, true);
        }
        return mask(std::move(out));
    }
    for (int i = amount; i < a.width; ++i) {
        if (bit(a, i)) setBit(out, i - amount, true);
    }
    if (sign) {
        for (int i = std::max(0, a.width - amount); i < width; ++i) setBit(out, i, true);
    }
    return mask(std::move(out));
}

EvalBits PredicateEvaluator::slice(const EvalBits& a, int hi, int lo) {
    EvalBits out = fromUInt64(0, hi >= lo ? hi - lo + 1 : 0);
    for (int i = lo; i <= hi; ++i) {
        if (bit(a, i)) setBit(out, i - lo, true);
    }
    return out;
}

EvalBits PredicateEvaluator::writeSlice(EvalBits base, int hi, int lo, const EvalBits& value) {
    if (hi < lo) std::swap(hi, lo);
    for (int i = lo; i <= hi; ++i) {
        setBit(base, i, bit(value, i - lo));
    }
    return mask(std::move(base));
}

EvalBits PredicateEvaluator::writeBit(EvalBits base, int index, const EvalBits& value) {
    setBit(base, index, bit(value, 0));
    return mask(std::move(base));
}

EvalBits PredicateEvaluator::concat(const std::vector<EvalBits>& parts) {
    int width = 0;
    for (const auto& part : parts) width += part.width;
    EvalBits out = fromUInt64(0, width);
    int offset = width;
    for (const auto& part : parts) {
        offset -= part.width;
        for (int i = 0; i < part.width; ++i) {
            if (bit(part, i)) setBit(out, offset + i, true);
        }
    }
    return out;
}

EvalBits PredicateEvaluator::signExtend(EvalBits value, int width) {
    bool sign = bit(value, value.width - 1);
    int old_width = value.width;
    value.width = width;
    value.is_signed = true;
    value.limbs.resize(static_cast<size_t>(limbCount(width)), 0);
    if (sign) {
        for (int i = old_width; i < width; ++i) setBit(value, i, true);
    }
    return mask(std::move(value));
}

EvalBits PredicateEvaluator::twosComplement(EvalBits value) {
    int width = value.width;
    bool is_signed = value.is_signed;
    for (auto& limb : value.limbs) limb = ~limb;
    return add(mask(std::move(value)), fromUInt64(1, width, is_signed), width);
}

int PredicateEvaluator::compareUnsigned(const EvalBits& a, const EvalBits& b) {
    int width = std::max(a.width, b.width);
    int limbs = limbCount(width);
    for (int i = limbs - 1; i >= 0; --i) {
        std::uint64_t av = i < static_cast<int>(a.limbs.size()) ? a.limbs[static_cast<size_t>(i)] : 0;
        std::uint64_t bv = i < static_cast<int>(b.limbs.size()) ? b.limbs[static_cast<size_t>(i)] : 0;
        if (i == limbs - 1 && width % 64 != 0) {
            std::uint64_t mask_bits = (std::uint64_t{1} << (width % 64)) - 1;
            av &= mask_bits;
            bv &= mask_bits;
        }
        if (av < bv) return -1;
        if (av > bv) return 1;
    }
    return 0;
}

int PredicateEvaluator::compareSigned(const EvalBits& a, const EvalBits& b) {
    int width = std::max(a.width, b.width);
    auto lhs = signExtend(a, width);
    auto rhs = signExtend(b, width);
    bool an = bit(lhs, lhs.width - 1);
    bool bn = bit(rhs, rhs.width - 1);
    if (an != bn) return an ? -1 : 1;
    return compareUnsigned(lhs, rhs);
}

int PredicateEvaluator::shiftAmount(const EvalBits& value) {
    if (value.limbs.size() > 1) {
        for (size_t i = 1; i < value.limbs.size(); ++i) {
            if (value.limbs[i] != 0) return std::numeric_limits<int>::max();
        }
    }
    if (value.limbs.empty()) return 0;
    if (value.limbs[0] > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(value.limbs[0]);
}

bool PredicateEvaluator::signedType(const TypeInfo& type) {
    return type.is_signed || type.hw_kind == "signed_view";
}

} // namespace pred
