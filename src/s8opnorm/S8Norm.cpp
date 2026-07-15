#include "s8opnorm/S8Norm.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace pred::s8opnorm {
namespace {

using namespace pred::s7flatten;

ErrorContext makeContext(DebugLoc loc = {}, std::string note = {}) {
    ErrorContext context;
    context.stage = "s8opnorm";
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    context.note = std::move(note);
    return context;
}

[[noreturn]] void fail(const std::string& message, DebugLoc loc = {}) {
    throwRTLZZ(makeContext(std::move(loc)), message);
}

bool isBoolType(const S8Type& type) {
    return type.kind == S8TypeKind::Bool && type.width == 1;
}

std::string typeText(const S8Type& type) {
    return type.kind == S8TypeKind::Bool ? "bool" : ("u" + std::to_string(type.width));
}

bool typeEq(const S8Type& lhs, const S8Type& rhs) {
    return lhs.kind == rhs.kind && lhs.width == rhs.width;
}

bool typeSigned(const TypeInfo& type) {
    if (type.name == "bool" || type.hw_kind == "bool") return false;
    if (type.hw_kind == "Int") return true;
    if (type.hw_kind == "UInt") return false;
    if (type.name.rfind("Int<", 0) == 0) return true;
    if (type.name.rfind("UInt<", 0) == 0) return false;
    if (type.name == "unsigned int" || type.name == "uint8_t" ||
        type.name == "uint16_t" || type.name == "uint32_t" ||
        type.name == "uint64_t") {
        return false;
    }
    if (type.name == "int" || type.name == "int8_t" ||
        type.name == "int16_t" || type.name == "int32_t" ||
        type.name == "int64_t") {
        return true;
    }
    return type.is_signed;
}

S8Type normType(const TypeInfo& type, DebugLoc loc = {}) {
    if (type.is_array || type.is_pointer || type.is_reference ||
        !type.struct_name.empty()) {
        fail("Non-scalar type reached S8 operation normalize", loc);
    }
    if (type.name == "bool" || type.hw_kind == "bool") {
        return S8Type{S8TypeKind::Bool, 1};
    }
    if (type.width <= 0) {
        fail("Scalar type with unknown width reached S8 operation normalize", loc);
    }
    return S8Type{S8TypeKind::Int, type.width};
}

S8SymbolRole convertRole(S7SymbolRole role) {
    switch (role) {
    case S7SymbolRole::Local: return S8SymbolRole::Local;
    case S7SymbolRole::Port: return S8SymbolRole::Port;
    case S7SymbolRole::Temp: return S8SymbolRole::Temp;
    }
    return S8SymbolRole::Local;
}

S8TermKind convertTermKind(S7TermKind kind) {
    switch (kind) {
    case S7TermKind::Jump: return S8TermKind::Jump;
    case S7TermKind::Branch: return S8TermKind::Branch;
    case S7TermKind::Switch: return S8TermKind::Switch;
    case S7TermKind::Exit: return S8TermKind::Exit;
    case S7TermKind::Unreachable: return S8TermKind::Unreachable;
    }
    fail("Unknown S7 terminator kind");
}

int wordCount(int width) {
    return width <= 0 ? 0 : ((width + 63) / 64);
}

std::uint64_t highMask(int width) {
    int rem = width % 64;
    if (rem == 0) return ~std::uint64_t{0};
    return (std::uint64_t{1} << rem) - 1;
}

void trimToWidth(std::vector<std::uint64_t>& words, int width) {
    words.resize(static_cast<std::size_t>(wordCount(width)), 0);
    if (!words.empty()) words.back() &= highMask(width);
}

bool bitAt(const std::vector<std::uint64_t>& words, int bit) {
    if (bit < 0) return false;
    std::size_t word = static_cast<std::size_t>(bit / 64);
    if (word >= words.size()) return false;
    return ((words[word] >> (bit % 64)) & 1U) != 0;
}

bool anyBitsAtOrAbove(const std::vector<std::uint64_t>& words, int bit) {
    if (bit < 0) return !words.empty();
    std::size_t word = static_cast<std::size_t>(bit / 64);
    int off = bit % 64;
    if (word < words.size()) {
        std::uint64_t mask = off == 0 ? ~std::uint64_t{0} : (~std::uint64_t{0} << off);
        if ((words[word] & mask) != 0) return true;
        ++word;
    }
    for (; word < words.size(); ++word) {
        if (words[word] != 0) return true;
    }
    return false;
}

bool fitsUnsignedWidth(const std::vector<std::uint64_t>& words, int width) {
    return !anyBitsAtOrAbove(words, width);
}

void mulSmall(std::vector<std::uint64_t>& words, std::uint32_t base) {
    unsigned __int128 carry = 0;
    for (auto& word : words) {
        unsigned __int128 value = static_cast<unsigned __int128>(word) * base + carry;
        word = static_cast<std::uint64_t>(value);
        carry = value >> 64;
    }
    if (carry != 0) words.push_back(static_cast<std::uint64_t>(carry));
}

void addSmall(std::vector<std::uint64_t>& words, std::uint32_t digit) {
    unsigned __int128 carry = digit;
    for (auto& word : words) {
        unsigned __int128 value = static_cast<unsigned __int128>(word) + carry;
        word = static_cast<std::uint64_t>(value);
        carry = value >> 64;
        if (carry == 0) return;
    }
    if (carry != 0) words.push_back(static_cast<std::uint64_t>(carry));
}

void twosComplement(std::vector<std::uint64_t>& words, int width) {
    trimToWidth(words, width);
    for (auto& word : words) word = ~word;
    addSmall(words, 1);
    trimToWidth(words, width);
}

struct LiteralText {
    bool negative = false;
    bool suffix_unsigned = false;
    std::string digits;
    int base = 10;
};

bool isIntegerSuffixChar(char c) {
    return c == 'u' || c == 'U' || c == 'l' || c == 'L' || c == 'z' || c == 'Z';
}

LiteralText splitLiteral(std::string text, DebugLoc loc) {
    LiteralText out;
    text.erase(std::remove(text.begin(), text.end(), '\''), text.end());
    if (text == "true" || text == "false") {
        out.digits = text == "true" ? "1" : "0";
        out.base = 10;
        return out;
    }
    if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
        out.negative = text[0] == '-';
        text.erase(text.begin());
    }
    while (!text.empty() && isIntegerSuffixChar(text.back())) {
        if (text.back() == 'u' || text.back() == 'U') out.suffix_unsigned = true;
        text.pop_back();
    }
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        out.base = 16;
        out.digits = text.substr(2);
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        out.base = 2;
        out.digits = text.substr(2);
    } else {
        out.base = 10;
        out.digits = text;
    }
    if (out.digits.empty()) fail("Empty integer literal", loc);
    return out;
}

std::uint32_t digitValue(char c, int base, DebugLoc loc) {
    int value = -1;
    if (c >= '0' && c <= '9') value = c - '0';
    else if (c >= 'a' && c <= 'f') value = 10 + (c - 'a');
    else if (c >= 'A' && c <= 'F') value = 10 + (c - 'A');
    if (value < 0 || value >= base) fail("Invalid digit in integer literal", loc);
    return static_cast<std::uint32_t>(value);
}

S8Literal parseLiteral(const std::string& text,
                       S8Type type,
                       bool signed_hint,
                       DebugLoc loc) {
    if (type.width <= 0) fail("Cannot parse literal with unknown width", loc);
    LiteralText split = splitLiteral(text, loc);
    if (type.kind == S8TypeKind::Bool &&
        !(text == "true" || text == "false" || text == "0" || text == "1" ||
          text == "0u" || text == "1u" || text == "0U" || text == "1U")) {
        fail("Bool literal must be true/false/0/1", loc);
    }

    std::vector<std::uint64_t> words(1, 0);
    for (char c : split.digits) {
        mulSmall(words, static_cast<std::uint32_t>(split.base));
        addSmall(words, digitValue(c, split.base, loc));
    }

    if (!fitsUnsignedWidth(words, type.width)) {
        fail("Integer literal exceeds target width", loc);
    }
    if (split.negative) twosComplement(words, type.width);
    trimToWidth(words, type.width);

    S8Literal out;
    out.words = std::move(words);
    out.valid_width = type.width;
    out.is_signed = signed_hint && !split.suffix_unsigned;
    out.source_text = text;
    return out;
}

std::string wordsHex(const std::vector<std::uint64_t>& words, int width) {
    if (width <= 0) return "0";
    std::ostringstream os;
    os << "0x";
    bool started = false;
    for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i) {
        std::ostringstream part;
        part << std::hex << words[static_cast<std::size_t>(i)];
        std::string text = part.str();
        if (!started) {
            os << text;
            started = true;
        } else {
            os << std::string(16 - std::min<int>(16, text.size()), '0') << text;
        }
    }
    if (!started) os << "0";
    return os.str();
}

struct Value {
    S8Operand operand;
};

struct Context {
    S8NormCFG output;
    NormOptions options;
    NormSummary summary;
    int temp_counter = 0;
};

const S8Symbol& symbolAt(const S8NormCFG& fn, SymbolId id) {
    if (id < 0 || id >= static_cast<SymbolId>(fn.symbols.size())) {
        fail("Invalid S8 symbol reference");
    }
    const auto& symbol = fn.symbols[static_cast<std::size_t>(id)];
    if (symbol.id != id) fail("Broken S8 symbol table invariant");
    return symbol;
}

S8Type symbolType(const S8NormCFG& fn, SymbolId id) {
    return symbolAt(fn, id).type;
}

S8Operand varOperand(const S8NormCFG& fn,
                     SymbolId symbol,
                     bool signed_view,
                     DebugLoc loc = {}) {
    S8Operand out;
    out.kind = S8OperandKind::Var;
    out.type = symbolType(fn, symbol);
    out.signed_view = signed_view;
    out.debug_loc = std::move(loc);
    out.symbol = symbol;
    return out;
}

SymbolId createTemp(Context& ctx, S8Type type, const std::string& hint) {
    if (static_cast<int>(ctx.output.symbols.size() + 1) > ctx.options.max_symbols) {
        fail("S8 symbol limit exceeded while creating temporary");
    }
    S8Symbol symbol;
    symbol.id = static_cast<SymbolId>(ctx.output.symbols.size());
    symbol.type = type;
    symbol.debug_name = "__s8_norm_" + hint + "_" + std::to_string(ctx.temp_counter++);
    symbol.role = S8SymbolRole::Temp;
    ctx.output.symbols.push_back(symbol);
    return symbol.id;
}

S8Operand normalizeOperand(Context& ctx, const S7Operand& operand) {
    S8Operand out;
    out.type = normType(operand.type, operand.debug_loc);
    out.signed_view = typeSigned(operand.type);
    out.debug_loc = operand.debug_loc;
    if (operand.kind == S7OperandKind::Literal) {
        out.kind = S8OperandKind::Literal;
        out.literal = parseLiteral(operand.literal_value, out.type, out.signed_view,
                                   operand.debug_loc);
        ++ctx.summary.parsed_literals;
        return out;
    }
    out.kind = S8OperandKind::Var;
    out.symbol = operand.symbol;
    if (!typeEq(out.type, symbolType(ctx.output, out.symbol))) {
        fail("S7 operand type does not match S8 symbol type", operand.debug_loc);
    }
    return out;
}

S8Stmt makeAssign(SymbolId target, S8Operand value, DebugLoc loc = {}) {
    S8Stmt stmt;
    stmt.kind = S8StmtKind::Assign;
    stmt.debug_loc = std::move(loc);
    stmt.target = target;
    stmt.value = std::move(value);
    return stmt;
}

S8Stmt makeOp(SymbolId target, S8Operation op, DebugLoc loc = {}) {
    S8Stmt stmt;
    stmt.kind = S8StmtKind::Op;
    stmt.debug_loc = std::move(loc);
    stmt.target = target;
    stmt.op = std::move(op);
    return stmt;
}

S8Operation unaryOp(S8OpKind kind, S8Operand operand, int result_width, DebugLoc loc = {}) {
    S8Operation op;
    op.kind = kind;
    op.debug_loc = std::move(loc);
    op.result_width = result_width;
    op.operands.push_back(std::move(operand));
    return op;
}

Value castTo(Context& ctx,
             S8Operand value,
             S8Type target_type,
             std::vector<S8Stmt>& out,
             DebugLoc loc,
             std::optional<SymbolId> target_symbol = std::nullopt) {
    if (value.type.width <= 0 || target_type.width <= 0) fail("Invalid cast width", loc);
    if (typeEq(value.type, target_type)) {
        if (target_symbol) {
            out.push_back(makeAssign(*target_symbol, value, loc));
            value = varOperand(ctx.output, *target_symbol, value.signed_view, loc);
        }
        return Value{std::move(value)};
    }

    SymbolId target = target_symbol.value_or(createTemp(ctx, target_type, "cast"));
    S8Operation op;
    op.debug_loc = loc;
    op.result_width = target_type.width;
    op.operands.push_back(value);
    if (value.type.kind != S8TypeKind::Bool && target_type.kind == S8TypeKind::Bool) {
        op.kind = S8OpKind::ReduceOr;
    } else if (value.type.width < target_type.width) {
        op.kind = value.signed_view ? S8OpKind::SExt : S8OpKind::ZExt;
    } else if (value.type.width > target_type.width) {
        op.kind = S8OpKind::Trunc;
    } else {
        out.push_back(makeAssign(target, value, loc));
        return Value{varOperand(ctx.output, target, value.signed_view, loc)};
    }
    out.push_back(makeOp(target, std::move(op), loc));
    ++ctx.summary.inserted_casts;
    return Value{varOperand(ctx.output, target, value.signed_view, loc)};
}

Value castToBool(Context& ctx, S8Operand value, std::vector<S8Stmt>& out, DebugLoc loc) {
    return castTo(ctx, std::move(value), S8Type{S8TypeKind::Bool, 1}, out, loc);
}

Value materializeOp(Context& ctx,
                    S8OpKind kind,
                    S8Type result_type,
                    std::vector<S8Operand> operands,
                    std::vector<S8Stmt>& out,
                    DebugLoc loc,
                    const std::string& hint) {
    SymbolId temp = createTemp(ctx, result_type, hint);
    S8Operation op;
    op.kind = kind;
    op.debug_loc = loc;
    op.result_width = result_type.width;
    op.operands = std::move(operands);
    out.push_back(makeOp(temp, std::move(op), loc));
    ++ctx.summary.normalized_ops;
    return Value{varOperand(ctx.output, temp, false, loc)};
}

Value normalizeUnary(Context& ctx,
                     const S7Operation& op,
                     S8Type target_type,
                     std::vector<S8Stmt>& out) {
    if (op.operands.size() != 1) fail("Malformed unary op", op.debug_loc);
    S8Operand operand = normalizeOperand(ctx, op.operands[0]);
    switch (op.unary_op) {
    case S7UnaryOp::LogicalNot: {
        auto bool_value = castToBool(ctx, std::move(operand), out, op.debug_loc).operand;
        return materializeOp(ctx, S8OpKind::LogicalNot, S8Type{S8TypeKind::Bool, 1},
                             {std::move(bool_value)}, out, op.debug_loc, "not");
    }
    case S7UnaryOp::BitNot: {
        auto v = castTo(ctx, std::move(operand), target_type, out, op.debug_loc).operand;
        return materializeOp(ctx, S8OpKind::BitNot, target_type, {std::move(v)},
                             out, op.debug_loc, "bitnot");
    }
    case S7UnaryOp::Negate: {
        auto v = castTo(ctx, std::move(operand), target_type, out, op.debug_loc).operand;
        return materializeOp(ctx, S8OpKind::Neg, target_type, {std::move(v)},
                             out, op.debug_loc, "neg");
    }
    case S7UnaryOp::Plus:
        return castTo(ctx, std::move(operand), target_type, out, op.debug_loc);
    }
    fail("Unknown unary op", op.debug_loc);
}

int addResultWidth(const S8Operand& lhs, const S8Operand& rhs) {
    return std::max(lhs.type.width, rhs.type.width) + 1;
}

int subResultWidth(const S8Operand& lhs, const S8Operand& rhs) {
    return std::max(lhs.type.width, rhs.type.width);
}

int mulResultWidth(const S8Operand& lhs, const S8Operand& rhs) {
    return lhs.type.width + rhs.type.width;
}

std::optional<std::uint64_t> literalUInt64(const S8Operand& operand) {
    if (operand.kind != S8OperandKind::Literal) return std::nullopt;
    if (operand.literal.words.size() > 1) {
        for (std::size_t i = 1; i < operand.literal.words.size(); ++i) {
            if (operand.literal.words[i] != 0) return std::nullopt;
        }
    }
    return operand.literal.words.empty() ? 0 : operand.literal.words.front();
}

std::uint64_t widthMask64(int width) {
    if (width >= 64) return ~std::uint64_t{0};
    return (std::uint64_t{1} << width) - 1;
}

std::optional<std::uint64_t> literalAbsUInt64(const S8Operand& operand) {
    auto raw = literalUInt64(operand);
    if (!raw) return std::nullopt;
    if (!operand.signed_view) return *raw;
    if (operand.type.width <= 0 || operand.type.width > 64) return std::nullopt;
    std::uint64_t masked = *raw & widthMask64(operand.type.width);
    if (!bitAt({masked}, operand.type.width - 1)) return masked;
    return ((~masked) + 1) & widthMask64(operand.type.width);
}

bool literalIsNegative(const S8Operand& operand) {
    if (!operand.signed_view || operand.kind != S8OperandKind::Literal) return false;
    auto raw = literalUInt64(operand);
    if (!raw || operand.type.width <= 0 || operand.type.width > 64) return false;
    return ((*raw >> (operand.type.width - 1)) & 1U) != 0;
}

bool isPowerOfTwo(std::uint64_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

bool divisorExceedsUnsignedRange(std::uint64_t divisor, int width) {
    if (width >= 64) return false;
    return divisor >= (std::uint64_t{1} << width);
}

int log2PowerOfTwo(std::uint64_t value) {
    int out = 0;
    while (value > 1) {
        value >>= 1;
        ++out;
    }
    return out;
}

S8Literal makeLiteralValue(std::uint64_t value, S8Type type, bool signed_view = false) {
    S8Literal literal;
    literal.valid_width = type.width;
    literal.is_signed = signed_view;
    literal.source_text = std::to_string(value);
    literal.words.push_back(value);
    trimToWidth(literal.words, type.width);
    return literal;
}

S8Operand constOperand(std::uint64_t value,
                       S8Type type,
                       bool signed_view = false,
                       DebugLoc loc = {}) {
    S8Operand out;
    out.kind = S8OperandKind::Literal;
    out.type = type;
    out.signed_view = signed_view;
    out.debug_loc = std::move(loc);
    out.literal = makeLiteralValue(value, type, signed_view);
    return out;
}

S8Operand boolConstOperand(bool value, DebugLoc loc = {}) {
    return constOperand(value ? 1 : 0, S8Type{S8TypeKind::Bool, 1}, false, loc);
}

struct UnsignedMagic {
    std::uint64_t multiplier = 0;
    bool add_indicator = false;
    int shift = 0;
};

UnsignedMagic computeUnsignedMagic(std::uint64_t divisor, int width, DebugLoc loc) {
    if (width <= 0 || width > 64) {
        fail("Constant division lowering currently supports widths up to 64 bits", loc);
    }
    if (divisor == 0) fail("Division by zero", loc);
    if (isPowerOfTwo(divisor)) return UnsignedMagic{0, false, log2PowerOfTwo(divisor)};

    const unsigned __int128 one = 1;
    const unsigned __int128 two_w = one << width;
    const unsigned __int128 nc = (two_w - 1) - ((two_w - 1) % divisor);
    int p = width - 1;
    unsigned __int128 q1 = (one << (width - 1)) / nc;
    unsigned __int128 r1 = (one << (width - 1)) - q1 * nc;
    unsigned __int128 q2 = ((one << (width - 1)) - 1) / divisor;
    unsigned __int128 r2 = ((one << (width - 1)) - 1) - q2 * divisor;
    bool add_indicator = false;
    while (true) {
        ++p;
        if (r1 >= nc - r1) {
            q1 = 2 * q1 + 1;
            r1 = 2 * r1 - nc;
        } else {
            q1 = 2 * q1;
            r1 = 2 * r1;
        }
        if (r2 + 1 >= divisor - r2) {
            if (q2 >= two_w - 1) add_indicator = true;
            q2 = 2 * q2 + 1;
            r2 = 2 * r2 + 1 - divisor;
        } else {
            if (q2 >= (one << (width - 1))) add_indicator = true;
            q2 = 2 * q2;
            r2 = 2 * r2 + 1;
        }
        const unsigned __int128 delta = divisor - 1 - r2;
        if (!(p < 2 * width && (q1 < delta || (q1 == delta && r1 == 0)))) break;
    }

    unsigned __int128 multiplier = q2 + 1;
    if (add_indicator) {
        if (multiplier < two_w) {
            fail("Internal error: constant division magic missing overflow multiplier", loc);
        }
        multiplier -= two_w;
    }
    if (multiplier > static_cast<unsigned __int128>(~std::uint64_t{0})) {
        fail("Constant division magic multiplier is too wide", loc);
    }
    return UnsignedMagic{static_cast<std::uint64_t>(multiplier),
                         add_indicator,
                         p - width};
}

Value makeShiftRight(Context& ctx,
                     S8Operand value,
                     int amount,
                     bool arithmetic,
                     std::vector<S8Stmt>& out,
                     DebugLoc loc,
                     const std::string& hint) {
    if (amount <= 0) return Value{std::move(value)};
    S8Type result_type = value.type;
    S8Operand shift = constOperand(static_cast<std::uint64_t>(amount),
                                   S8Type{S8TypeKind::Int, 64}, false, loc);
    S8OpKind kind = arithmetic ? S8OpKind::AShr : S8OpKind::LShr;
    return materializeOp(ctx, kind, result_type, {std::move(value), std::move(shift)},
                         out, loc, hint);
}

Value makeSlice(Context& ctx,
                S8Operand value,
                int hi,
                int lo,
                std::vector<S8Stmt>& out,
                DebugLoc loc,
                const std::string& hint) {
    if (lo < 0 || hi < lo || hi >= value.type.width) fail("Constant division slice out of bounds", loc);
    S8Type result_type{S8TypeKind::Int, hi - lo + 1};
    Value sliced = materializeOp(ctx, S8OpKind::Slice, result_type, {std::move(value)},
                                 out, loc, hint);
    out.back().op.hi = hi;
    out.back().op.lo = lo;
    return sliced;
}

Value makeSignBit(Context& ctx,
                  S8Operand value,
                  std::vector<S8Stmt>& out,
                  DebugLoc loc) {
    int width = value.type.width;
    Value sign = materializeOp(ctx, S8OpKind::BitSelect, S8Type{S8TypeKind::Bool, 1},
                               {std::move(value)}, out, loc, "signbit");
    out.back().op.bit = width - 1;
    return sign;
}

Value makeBoolNot(Context& ctx,
                  S8Operand value,
                  std::vector<S8Stmt>& out,
                  DebugLoc loc) {
    return materializeOp(ctx, S8OpKind::LogicalNot, S8Type{S8TypeKind::Bool, 1},
                         {std::move(value)}, out, loc, "boolnot");
}

Value makeNegWord(Context& ctx,
                  S8Operand value,
                  S8Type word_type,
                  std::vector<S8Stmt>& out,
                  DebugLoc loc,
                  const std::string& hint) {
    S8Operand v = castTo(ctx, std::move(value), word_type, out, loc).operand;
    return materializeOp(ctx, S8OpKind::Neg, word_type, {std::move(v)}, out, loc, hint);
}

Value makeMuxWord(Context& ctx,
                  S8Operand cond,
                  S8Operand then_value,
                  S8Operand else_value,
                  S8Type word_type,
                  std::vector<S8Stmt>& out,
                  DebugLoc loc,
                  const std::string& hint) {
    S8Operand t = castTo(ctx, std::move(then_value), word_type, out, loc).operand;
    S8Operand f = castTo(ctx, std::move(else_value), word_type, out, loc).operand;
    return materializeOp(ctx, S8OpKind::Mux, word_type,
                         {std::move(cond), std::move(t), std::move(f)}, out, loc, hint);
}

Value makeAbsSigned(Context& ctx,
                    S8Operand lhs,
                    std::vector<S8Stmt>& out,
                    DebugLoc loc,
                    std::optional<S8Operand>& negative_out) {
    S8Type word_type = lhs.type;
    lhs.signed_view = false;
    Value sign = makeSignBit(ctx, lhs, out, loc);
    negative_out = sign.operand;
    Value neg = makeNegWord(ctx, lhs, word_type, out, loc, "abs_neg");
    return makeMuxWord(ctx, sign.operand, std::move(neg.operand), std::move(lhs),
                       word_type, out, loc, "abs");
}

Value applyConditionalNeg(Context& ctx,
                          S8Operand value,
                          S8Operand negative,
                          S8Type word_type,
                          std::vector<S8Stmt>& out,
                          DebugLoc loc,
                          const std::string& hint) {
    S8Operand v = castTo(ctx, std::move(value), word_type, out, loc).operand;
    if (negative.kind == S8OperandKind::Literal) {
        auto raw = literalUInt64(negative);
        if (raw && (*raw & 1U) == 0) return Value{std::move(v)};
        return makeNegWord(ctx, std::move(v), word_type, out, loc, hint + "_neg");
    }
    Value neg = makeNegWord(ctx, v, word_type, out, loc, hint + "_neg");
    return makeMuxWord(ctx, std::move(negative), std::move(neg.operand), std::move(v),
                       word_type, out, loc, hint + "_mux");
}

S8Operand quotientSign(Context& ctx,
                       std::optional<S8Operand> lhs_negative,
                       bool divisor_negative,
                       std::vector<S8Stmt>& out,
                       DebugLoc loc) {
    if (!lhs_negative) return boolConstOperand(divisor_negative, loc);
    if (!divisor_negative) return *lhs_negative;
    return makeBoolNot(ctx, *lhs_negative, out, loc).operand;
}

Value lowerUnsignedDivConst(Context& ctx,
                            S8Operand lhs,
                            std::uint64_t divisor,
                            std::vector<S8Stmt>& out,
                            DebugLoc loc) {
    if (lhs.signed_view) fail("Internal error: unsigned constant division received signed operand view", loc);
    if (divisor == 0) fail("Division by zero", loc);
    const int width = lhs.type.width;
    if (divisorExceedsUnsignedRange(divisor, width)) {
        return Value{constOperand(0, S8Type{S8TypeKind::Int, width}, false, loc)};
    }
    if (divisor == 1) return Value{std::move(lhs)};
    if (isPowerOfTwo(divisor)) {
        int shift = log2PowerOfTwo(divisor);
        if (shift >= width) {
            return Value{constOperand(0, S8Type{S8TypeKind::Int, width}, false, loc)};
        }
        return makeSlice(ctx, std::move(lhs), width - 1, shift, out, loc, "divpow2");
    }

    UnsignedMagic magic = computeUnsignedMagic(divisor, width, loc);
    S8Type word_type{S8TypeKind::Int, width};
    S8Operand magic_operand = constOperand(magic.multiplier, word_type, false, loc);
    Value product = materializeOp(ctx, S8OpKind::Mul,
                                  S8Type{S8TypeKind::Int, width * 2},
                                  {lhs, std::move(magic_operand)}, out, loc, "divmul");
    Value high = makeSlice(ctx, std::move(product.operand), width * 2 - 1, width,
                           out, loc, "divhigh");
    if (!magic.add_indicator) {
        return makeShiftRight(ctx, std::move(high.operand), magic.shift, false,
                              out, loc, "divshift");
    }

    Value diff = materializeOp(ctx, S8OpKind::Sub, word_type,
                               {std::move(lhs), high.operand}, out, loc, "divcorr_sub");
    Value half = makeShiftRight(ctx, std::move(diff.operand), 1, false,
                                out, loc, "divcorr_half");
    Value sum = materializeOp(ctx, S8OpKind::Add,
                              S8Type{S8TypeKind::Int, width + 1},
                              {std::move(half.operand), std::move(high.operand)},
                              out, loc, "divcorr_add");
    return makeShiftRight(ctx, std::move(sum.operand), magic.shift - 1, false,
                          out, loc, "divcorr_shift");
}

Value lowerUnsignedModConst(Context& ctx,
                            S8Operand lhs,
                            std::uint64_t divisor,
                            std::vector<S8Stmt>& out,
                            DebugLoc loc) {
    if (lhs.signed_view) fail("Internal error: unsigned constant modulo received signed operand view", loc);
    if (divisor == 0) fail("Modulo by zero", loc);
    const int width = lhs.type.width;
    S8Type word_type{S8TypeKind::Int, width};
    if (divisorExceedsUnsignedRange(divisor, width)) return Value{std::move(lhs)};
    if (divisor == 1) return Value{constOperand(0, word_type, false, loc)};
    if (isPowerOfTwo(divisor)) {
        int bits = log2PowerOfTwo(divisor);
        if (bits == 0) return Value{constOperand(0, word_type, false, loc)};
        return makeSlice(ctx, std::move(lhs), bits - 1, 0, out, loc, "modpow2");
    }

    S8Operand lhs_for_div = lhs;
    Value quotient = lowerUnsignedDivConst(ctx, std::move(lhs_for_div), divisor, out, loc);
    S8Operand q_word = castTo(ctx, std::move(quotient.operand), word_type, out, loc).operand;
    S8Operand divisor_operand = constOperand(divisor, word_type, false, loc);
    Value product = materializeOp(ctx, S8OpKind::Mul,
                                  S8Type{S8TypeKind::Int, width * 2},
                                  {std::move(q_word), std::move(divisor_operand)},
                                  out, loc, "modmul");
    S8Operand product_low = castTo(ctx, std::move(product.operand), word_type, out, loc).operand;
    return materializeOp(ctx, S8OpKind::Sub, word_type,
                         {std::move(lhs), std::move(product_low)}, out, loc, "modsub");
}

Value lowerSignedDivConst(Context& ctx,
                          S8Operand lhs,
                          const S8Operand& divisor_operand,
                          std::vector<S8Stmt>& out,
                          DebugLoc loc) {
    auto abs_divisor = literalAbsUInt64(divisor_operand);
    if (!abs_divisor) fail("Signed constant division divisor is too wide", loc);
    if (*abs_divisor == 0) fail("Division by zero", loc);
    S8Type word_type = lhs.type;
    std::optional<S8Operand> lhs_negative;
    bool lhs_signed = lhs.signed_view;
    lhs.signed_view = false;
    Value abs_lhs = lhs_signed
        ? makeAbsSigned(ctx, lhs, out, loc, lhs_negative)
        : Value{lhs};
    bool divisor_negative = literalIsNegative(divisor_operand);
    Value abs_quotient = lowerUnsignedDivConst(ctx, std::move(abs_lhs.operand),
                                               *abs_divisor, out, loc);
    S8Operand sign = quotientSign(ctx, lhs_negative, divisor_negative, out, loc);
    return applyConditionalNeg(ctx, std::move(abs_quotient.operand), std::move(sign),
                               word_type, out, loc, "sdiv");
}

Value lowerSignedModConst(Context& ctx,
                          S8Operand lhs,
                          const S8Operand& divisor_operand,
                          std::vector<S8Stmt>& out,
                          DebugLoc loc) {
    auto abs_divisor = literalAbsUInt64(divisor_operand);
    if (!abs_divisor) fail("Signed constant modulo divisor is too wide", loc);
    if (*abs_divisor == 0) fail("Modulo by zero", loc);
    S8Type word_type = lhs.type;
    std::optional<S8Operand> lhs_negative;
    bool lhs_signed = lhs.signed_view;
    lhs.signed_view = false;
    Value abs_lhs = lhs_signed
        ? makeAbsSigned(ctx, lhs, out, loc, lhs_negative)
        : Value{lhs};
    Value abs_remainder = lowerUnsignedModConst(ctx, std::move(abs_lhs.operand),
                                                *abs_divisor, out, loc);
    S8Operand sign = lhs_negative ? *lhs_negative : boolConstOperand(false, loc);
    return applyConditionalNeg(ctx, std::move(abs_remainder.operand), std::move(sign),
                               word_type, out, loc, "smod");
}

Value normalizeBinary(Context& ctx,
                      const S7Operation& op,
                      S8Type target_type,
                      std::vector<S8Stmt>& out) {
    if (op.operands.size() != 2) fail("Malformed binary op", op.debug_loc);
    S8Operand lhs = normalizeOperand(ctx, op.operands[0]);
    S8Operand rhs = normalizeOperand(ctx, op.operands[1]);
    switch (op.binary_op) {
    case S7BinaryOp::Div:
    case S7BinaryOp::Mod: {
        auto divisor = literalUInt64(rhs);
        if (!divisor) {
            fail("Div/Mod are only supported when the second operand is a constant", op.debug_loc);
        }
        if (lhs.signed_view || rhs.signed_view) {
            if (op.binary_op == S7BinaryOp::Div) {
                return lowerSignedDivConst(ctx, std::move(lhs), rhs, out, op.debug_loc);
            }
            return lowerSignedModConst(ctx, std::move(lhs), rhs, out, op.debug_loc);
        }
        if (op.binary_op == S7BinaryOp::Div) {
            return lowerUnsignedDivConst(ctx, std::move(lhs), *divisor, out, op.debug_loc);
        }
        return lowerUnsignedModConst(ctx, std::move(lhs), *divisor, out, op.debug_loc);
    }
    case S7BinaryOp::Add: {
        S8Type op_type{S8TypeKind::Int, addResultWidth(lhs, rhs)};
        auto l = castTo(ctx, lhs, op_type, out, op.debug_loc).operand;
        auto r = castTo(ctx, rhs, op_type, out, op.debug_loc).operand;
        return materializeOp(ctx, S8OpKind::Add, op_type, {std::move(l), std::move(r)},
                             out, op.debug_loc, "add");
    }
    case S7BinaryOp::Sub: {
        S8Type op_type{S8TypeKind::Int, subResultWidth(lhs, rhs)};
        auto l = castTo(ctx, lhs, op_type, out, op.debug_loc).operand;
        auto r = castTo(ctx, rhs, op_type, out, op.debug_loc).operand;
        return materializeOp(ctx, S8OpKind::Sub, op_type, {std::move(l), std::move(r)},
                             out, op.debug_loc, "sub");
    }
    case S7BinaryOp::Mul: {
        S8Type op_type{S8TypeKind::Int, mulResultWidth(lhs, rhs)};
        return materializeOp(ctx, S8OpKind::Mul, op_type, {std::move(lhs), std::move(rhs)},
                             out, op.debug_loc, "mul");
    }
    case S7BinaryOp::BitAnd:
    case S7BinaryOp::BitOr:
    case S7BinaryOp::BitXor: {
        int width = std::max({lhs.type.width, rhs.type.width, target_type.width});
        S8Type op_type{S8TypeKind::Int, width};
        auto l = castTo(ctx, lhs, op_type, out, op.debug_loc).operand;
        auto r = castTo(ctx, rhs, op_type, out, op.debug_loc).operand;
        S8OpKind kind = op.binary_op == S7BinaryOp::BitAnd ? S8OpKind::BitAnd :
            (op.binary_op == S7BinaryOp::BitOr ? S8OpKind::BitOr : S8OpKind::BitXor);
        return materializeOp(ctx, kind, op_type, {std::move(l), std::move(r)},
                             out, op.debug_loc, "bitop");
    }
    case S7BinaryOp::LogicalAnd:
    case S7BinaryOp::LogicalOr: {
        auto l = castToBool(ctx, lhs, out, op.debug_loc).operand;
        auto r = castToBool(ctx, rhs, out, op.debug_loc).operand;
        return materializeOp(ctx,
                             op.binary_op == S7BinaryOp::LogicalAnd ?
                                 S8OpKind::BoolAnd : S8OpKind::BoolOr,
                             S8Type{S8TypeKind::Bool, 1},
                             {std::move(l), std::move(r)}, out, op.debug_loc, "logic");
    }
    case S7BinaryOp::Shl:
    case S7BinaryOp::Shr: {
        auto l = castTo(ctx, lhs, target_type, out, op.debug_loc).operand;
        S8OpKind kind = op.binary_op == S7BinaryOp::Shl ? S8OpKind::Shl :
            (l.signed_view ? S8OpKind::AShr : S8OpKind::LShr);
        return materializeOp(ctx, kind, target_type, {std::move(l), std::move(rhs)},
                             out, op.debug_loc, "shift");
    }
    case S7BinaryOp::Eq:
    case S7BinaryOp::Ne:
    case S7BinaryOp::Lt:
    case S7BinaryOp::Le:
    case S7BinaryOp::Gt:
    case S7BinaryOp::Ge: {
        int width = std::max(lhs.type.width, rhs.type.width);
        S8Type cmp_type{S8TypeKind::Int, width};
        auto l = castTo(ctx, lhs, cmp_type, out, op.debug_loc).operand;
        auto r = castTo(ctx, rhs, cmp_type, out, op.debug_loc).operand;
        S8OpKind kind = S8OpKind::Eq;
        if (op.binary_op == S7BinaryOp::Ne) kind = S8OpKind::Ne;
        else if (op.binary_op == S7BinaryOp::Lt) kind = S8OpKind::Lt;
        else if (op.binary_op == S7BinaryOp::Le) kind = S8OpKind::Le;
        else if (op.binary_op == S7BinaryOp::Gt) kind = S8OpKind::Gt;
        else if (op.binary_op == S7BinaryOp::Ge) kind = S8OpKind::Ge;
        return materializeOp(ctx, kind, S8Type{S8TypeKind::Bool, 1},
                             {std::move(l), std::move(r)}, out, op.debug_loc, "cmp");
    }
    }
    fail("Unknown binary op", op.debug_loc);
}

Value normalizeMux(Context& ctx,
                   const S7Operation& op,
                   S8Type target_type,
                   std::vector<S8Stmt>& out) {
    if (op.operands.size() != 3) fail("Malformed ternary op", op.debug_loc);
    auto cond = castToBool(ctx, normalizeOperand(ctx, op.operands[0]), out, op.debug_loc).operand;
    auto then_value = castTo(ctx, normalizeOperand(ctx, op.operands[1]), target_type,
                             out, op.debug_loc).operand;
    auto else_value = castTo(ctx, normalizeOperand(ctx, op.operands[2]), target_type,
                             out, op.debug_loc).operand;
    return materializeOp(ctx, S8OpKind::Mux, target_type,
                         {std::move(cond), std::move(then_value), std::move(else_value)},
                         out, op.debug_loc, "mux");
}

Value normalizeCast(Context& ctx,
                    const S7Operation& op,
                    S8Type target_type,
                    std::vector<S8Stmt>& out) {
    if (op.operands.size() != 1) fail("Malformed cast op", op.debug_loc);
    return castTo(ctx, normalizeOperand(ctx, op.operands[0]), target_type,
                  out, op.debug_loc);
}

S8Type hardwareRawType(const S7Operation& op, S8Type target_type,
                       const std::vector<S8Operand>& operands) {
    switch (op.hardware_op) {
    case S7HardwareOp::ZExt:
    case S7HardwareOp::SExt:
    case S7HardwareOp::Trunc:
        return target_type;
    case S7HardwareOp::Slice:
        return S8Type{S8TypeKind::Int, op.hi - op.lo + 1};
    case S7HardwareOp::BitSelect:
    case S7HardwareOp::DynamicBitSelect:
    case S7HardwareOp::ReduceOr:
    case S7HardwareOp::ReduceAnd:
    case S7HardwareOp::ReduceXor:
        return S8Type{S8TypeKind::Bool, 1};
    case S7HardwareOp::DynamicSlice:
        return target_type;
    case S7HardwareOp::WriteSlice:
    case S7HardwareOp::WriteBit:
    case S7HardwareOp::DynamicWriteSlice:
    case S7HardwareOp::DynamicWriteBit:
        return operands.empty() ? target_type : operands[0].type;
    case S7HardwareOp::Concat: {
        int width = 0;
        for (const auto& operand : operands) width += operand.type.width;
        return S8Type{S8TypeKind::Int, width};
    }
    case S7HardwareOp::Repeat:
        return operands.empty() ? target_type :
            S8Type{S8TypeKind::Int, operands[0].type.width * op.times};
    }
    fail("Unknown hardware op", op.debug_loc);
}

void checkArity(const S7Operation& op, std::size_t actual, std::size_t expected) {
    if (actual != expected) fail("Malformed hardware op operand count", op.debug_loc);
}

S8OpKind convertHardwareKind(S7HardwareOp op) {
    switch (op) {
    case S7HardwareOp::ZExt: return S8OpKind::ZExt;
    case S7HardwareOp::SExt: return S8OpKind::SExt;
    case S7HardwareOp::Trunc: return S8OpKind::Trunc;
    case S7HardwareOp::Slice: return S8OpKind::Slice;
    case S7HardwareOp::BitSelect: return S8OpKind::BitSelect;
    case S7HardwareOp::DynamicSlice: return S8OpKind::DynamicSlice;
    case S7HardwareOp::DynamicBitSelect: return S8OpKind::DynamicBitSelect;
    case S7HardwareOp::WriteSlice: return S8OpKind::WriteSlice;
    case S7HardwareOp::WriteBit: return S8OpKind::WriteBit;
    case S7HardwareOp::DynamicWriteSlice: return S8OpKind::DynamicWriteSlice;
    case S7HardwareOp::DynamicWriteBit: return S8OpKind::DynamicWriteBit;
    case S7HardwareOp::Concat: return S8OpKind::Concat;
    case S7HardwareOp::Repeat: return S8OpKind::Repeat;
    case S7HardwareOp::ReduceOr: return S8OpKind::ReduceOr;
    case S7HardwareOp::ReduceAnd: return S8OpKind::ReduceAnd;
    case S7HardwareOp::ReduceXor: return S8OpKind::ReduceXor;
    }
    fail("Unknown hardware op");
}

Value normalizeHardware(Context& ctx,
                        const S7Operation& op,
                        S8Type target_type,
                        std::vector<S8Stmt>& out) {
    std::vector<S8Operand> operands;
    operands.reserve(op.operands.size());
    for (const auto& operand : op.operands) operands.push_back(normalizeOperand(ctx, operand));

    switch (op.hardware_op) {
    case S7HardwareOp::ZExt:
        checkArity(op, operands.size(), 1);
        if (target_type.width < operands[0].type.width) fail("ZExt target narrower than source", op.debug_loc);
        break;
    case S7HardwareOp::SExt:
        checkArity(op, operands.size(), 1);
        if (target_type.width < operands[0].type.width) fail("SExt target narrower than source", op.debug_loc);
        operands[0].signed_view = true;
        break;
    case S7HardwareOp::Trunc:
        checkArity(op, operands.size(), 1);
        if (target_type.width > operands[0].type.width) fail("Trunc target wider than source", op.debug_loc);
        break;
    case S7HardwareOp::Slice:
        checkArity(op, operands.size(), 1);
        if (op.lo < 0 || op.hi < op.lo || op.hi >= operands[0].type.width) {
            fail("Slice out of bounds", op.debug_loc);
        }
        break;
    case S7HardwareOp::BitSelect:
        checkArity(op, operands.size(), 1);
        if (op.bit < 0 || op.bit >= operands[0].type.width) fail("BitSelect out of bounds", op.debug_loc);
        break;
    case S7HardwareOp::DynamicSlice:
        checkArity(op, operands.size(), 2);
        if (target_type.width <= 0 || target_type.width > operands[0].type.width) {
            fail("DynamicSlice width out of bounds", op.debug_loc);
        }
        operands[1].signed_view = false;
        break;
    case S7HardwareOp::DynamicBitSelect:
        checkArity(op, operands.size(), 2);
        operands[1].signed_view = false;
        break;
    case S7HardwareOp::WriteSlice:
        checkArity(op, operands.size(), 2);
        if (op.lo < 0 || op.hi < op.lo || op.hi >= operands[0].type.width) {
            fail("WriteSlice out of bounds", op.debug_loc);
        }
        operands[1] = castTo(ctx, operands[1], S8Type{S8TypeKind::Int, op.hi - op.lo + 1},
                             out, op.debug_loc).operand;
        break;
    case S7HardwareOp::WriteBit:
        checkArity(op, operands.size(), 2);
        if (op.bit < 0 || op.bit >= operands[0].type.width) fail("WriteBit out of bounds", op.debug_loc);
        operands[1] = castToBool(ctx, operands[1], out, op.debug_loc).operand;
        break;
    case S7HardwareOp::DynamicWriteSlice:
        checkArity(op, operands.size(), 3);
        if (operands[2].type.width > operands[0].type.width) {
            fail("DynamicWriteSlice value wider than base", op.debug_loc);
        }
        operands[1].signed_view = false;
        break;
    case S7HardwareOp::DynamicWriteBit:
        checkArity(op, operands.size(), 3);
        operands[1].signed_view = false;
        operands[2] = castToBool(ctx, operands[2], out, op.debug_loc).operand;
        break;
    case S7HardwareOp::Concat:
        if (operands.empty()) fail("Concat expects at least one operand", op.debug_loc);
        break;
    case S7HardwareOp::Repeat:
        checkArity(op, operands.size(), 1);
        if (op.times <= 0) fail("Repeat expects positive times", op.debug_loc);
        break;
    case S7HardwareOp::ReduceOr:
    case S7HardwareOp::ReduceAnd:
    case S7HardwareOp::ReduceXor:
        checkArity(op, operands.size(), 1);
        break;
    }

    S8Type raw_type = hardwareRawType(op, target_type, operands);
    Value raw = materializeOp(ctx, convertHardwareKind(op.hardware_op), raw_type,
                              std::move(operands), out, op.debug_loc, "hw");
    out.back().op.hi = op.hi;
    out.back().op.lo = op.lo;
    out.back().op.bit = op.bit;
    out.back().op.times = op.times;
    return raw;
}

Value normalizeOperation(Context& ctx,
                         const S7Operation& op,
                         S8Type target_type,
                         std::vector<S8Stmt>& out) {
    switch (op.kind) {
    case S7OpKind::Unary:
        return normalizeUnary(ctx, op, target_type, out);
    case S7OpKind::Binary:
        return normalizeBinary(ctx, op, target_type, out);
    case S7OpKind::Ternary:
        return normalizeMux(ctx, op, target_type, out);
    case S7OpKind::Cast:
        return normalizeCast(ctx, op, target_type, out);
    case S7OpKind::Hardware:
        return normalizeHardware(ctx, op, target_type, out);
    }
    fail("Unknown S7 op kind", op.debug_loc);
}

void emitCastToTarget(Context& ctx,
                      S8Operand value,
                      SymbolId target,
                      std::vector<S8Stmt>& out,
                      DebugLoc loc) {
    castTo(ctx, std::move(value), symbolType(ctx.output, target), out, loc, target);
}

S8Stmt normalizeLookup(Context& ctx, const S7Stmt& stmt, std::vector<S8Stmt>& out) {
    S8Stmt normalized;
    normalized.kind = S8StmtKind::Lookup;
    normalized.debug_loc = stmt.debug_loc;
    normalized.target = stmt.target;
    S8Type target_type = symbolType(ctx.output, stmt.target);
    normalized.lookup_index = normalizeOperand(ctx, stmt.lookup_index);
    normalized.lookup_index.signed_view = false;
    for (const auto& elem : stmt.lookup_elements) {
        normalized.lookup_elements.push_back(
            castTo(ctx, normalizeOperand(ctx, elem), target_type, out, stmt.debug_loc).operand);
    }
    return normalized;
}

S8Stmt normalizeLookupWrite(Context& ctx, const S7Stmt& stmt, std::vector<S8Stmt>& out) {
    if (stmt.lookup_write_targets.empty()) fail("LookupWrite has no targets", stmt.debug_loc);
    int width = 0;
    for (SymbolId target : stmt.lookup_write_targets) {
        width = std::max(width, symbolType(ctx.output, target).width);
    }
    S8Type wide{S8TypeKind::Int, width};
    S8Stmt normalized;
    normalized.kind = S8StmtKind::LookupWrite;
    normalized.debug_loc = stmt.debug_loc;
    normalized.lookup_index = normalizeOperand(ctx, stmt.lookup_index);
    normalized.lookup_index.signed_view = false;
    normalized.lookup_value = castTo(ctx, normalizeOperand(ctx, stmt.lookup_value),
                                     wide, out, stmt.debug_loc).operand;
    for (const auto& elem : stmt.lookup_elements) {
        normalized.lookup_elements.push_back(
            castTo(ctx, normalizeOperand(ctx, elem), wide, out, stmt.debug_loc).operand);
    }
    normalized.lookup_write_targets = stmt.lookup_write_targets;
    return normalized;
}

void normalizeStmt(Context& ctx, const S7Stmt& stmt, std::vector<S8Stmt>& out) {
    switch (stmt.kind) {
    case S7StmtKind::Assign:
        emitCastToTarget(ctx, normalizeOperand(ctx, stmt.value), stmt.target, out,
                         stmt.debug_loc);
        return;
    case S7StmtKind::Op: {
        S8Type target_type = symbolType(ctx.output, stmt.target);
        Value value = normalizeOperation(ctx, stmt.op, target_type, out);
        emitCastToTarget(ctx, std::move(value.operand), stmt.target, out, stmt.debug_loc);
        return;
    }
    case S7StmtKind::Lookup:
        out.push_back(normalizeLookup(ctx, stmt, out));
        return;
    case S7StmtKind::LookupWrite:
        out.push_back(normalizeLookupWrite(ctx, stmt, out));
        return;
    }
    fail("Unknown S7 stmt kind", stmt.debug_loc);
}

S8Terminator normalizeTerminator(Context& ctx,
                                 const S7Terminator& term,
                                 std::vector<S8Stmt>& prelude) {
    S8Terminator out;
    out.kind = convertTermKind(term.kind);
    out.jump_target = term.jump_target;
    out.true_target = term.true_target;
    out.false_target = term.false_target;
    out.default_target = term.default_target;
    if (term.kind == S7TermKind::Branch) {
        out.condition = castToBool(ctx, normalizeOperand(ctx, term.condition),
                                   prelude, term.condition.debug_loc).operand;
    } else if (term.kind == S7TermKind::Switch) {
        out.switch_value = normalizeOperand(ctx, term.switch_value);
        std::unordered_set<std::string> cases;
        for (const auto& target : term.switch_targets) {
            S8SwitchTarget normalized;
            normalized.target = target.target;
            if (target.value) {
                auto value = castTo(ctx, normalizeOperand(ctx, target.value.value()),
                                    out.switch_value.type, prelude,
                                    target.value->debug_loc).operand;
                if (value.kind == S8OperandKind::Literal) {
                    std::string key = wordsHex(value.literal.words, value.literal.valid_width);
                    if (!cases.insert(key).second) fail("Duplicate switch case after normalization", target.value->debug_loc);
                }
                normalized.value = std::move(value);
            }
            out.switch_targets.push_back(std::move(normalized));
        }
    }
    return out;
}

S8NormCFG normalizeFunction(const FlattenedCFG& fn, const NormOptions& options,
                            NormSummary& summary) {
    Context ctx;
    ctx.options = options;
    ctx.output.name = fn.name;
    ctx.output.entry = fn.entry;
    ctx.output.exit = fn.exit;
    ctx.summary.function_name = fn.name;
    ctx.output.symbols.reserve(fn.symbols.size());
    for (const auto& symbol : fn.symbols) {
        S8Symbol out;
        out.id = symbol.id;
        out.type = normType(symbol.type);
        out.debug_name = symbol.debug_name;
        out.role = convertRole(symbol.role);
        if (out.id != static_cast<SymbolId>(ctx.output.symbols.size())) {
            fail("S7 symbols must be dense before S8 operation normalize");
        }
        ctx.output.symbols.push_back(std::move(out));
    }
    for (const auto& port : fn.ports) {
        S8Port out;
        out.symbol = port.symbol;
        out.direction = port.direction;
        out.passing = port.passing;
        ctx.output.ports.push_back(out);
    }
    for (const auto& block : fn.blocks) {
        S8BasicBlock out_block;
        out_block.id = block.id;
        for (const auto& stmt : block.stmts) normalizeStmt(ctx, stmt, out_block.stmts);
        out_block.terminator = normalizeTerminator(ctx, block.terminator, out_block.stmts);
        ctx.output.blocks.push_back(std::move(out_block));
    }
    summary = ctx.summary;
    return std::move(ctx.output);
}

std::string symbolName(const S8NormCFG& fn, SymbolId symbol) {
    return symbolAt(fn, symbol).debug_name;
}

std::string operandText(const S8NormCFG& fn, const S8Operand& operand) {
    std::ostringstream os;
    if (operand.signed_view) os << "s:";
    if (operand.kind == S8OperandKind::Var) {
        os << symbolName(fn, operand.symbol);
    } else {
        os << wordsHex(operand.literal.words, operand.literal.valid_width);
    }
    os << "<" << typeText(operand.type) << ">";
    return os.str();
}

std::string opName(S8OpKind kind) {
    switch (kind) {
    case S8OpKind::AssignCast: return "AssignCast";
    case S8OpKind::Add: return "Add";
    case S8OpKind::Sub: return "Sub";
    case S8OpKind::Mul: return "Mul";
    case S8OpKind::Neg: return "Neg";
    case S8OpKind::BitNot: return "BitNot";
    case S8OpKind::LogicalNot: return "LogicalNot";
    case S8OpKind::BitAnd: return "BitAnd";
    case S8OpKind::BitOr: return "BitOr";
    case S8OpKind::BitXor: return "BitXor";
    case S8OpKind::BoolAnd: return "BoolAnd";
    case S8OpKind::BoolOr: return "BoolOr";
    case S8OpKind::Shl: return "Shl";
    case S8OpKind::LShr: return "LShr";
    case S8OpKind::AShr: return "AShr";
    case S8OpKind::Eq: return "Eq";
    case S8OpKind::Ne: return "Ne";
    case S8OpKind::Lt: return "Lt";
    case S8OpKind::Le: return "Le";
    case S8OpKind::Gt: return "Gt";
    case S8OpKind::Ge: return "Ge";
    case S8OpKind::Mux: return "Mux";
    case S8OpKind::ZExt: return "ZExt";
    case S8OpKind::SExt: return "SExt";
    case S8OpKind::Trunc: return "Trunc";
    case S8OpKind::Slice: return "Slice";
    case S8OpKind::BitSelect: return "BitSelect";
    case S8OpKind::DynamicSlice: return "DynamicSlice";
    case S8OpKind::DynamicBitSelect: return "DynamicBitSelect";
    case S8OpKind::WriteSlice: return "WriteSlice";
    case S8OpKind::WriteBit: return "WriteBit";
    case S8OpKind::DynamicWriteSlice: return "DynamicWriteSlice";
    case S8OpKind::DynamicWriteBit: return "DynamicWriteBit";
    case S8OpKind::Concat: return "Concat";
    case S8OpKind::Repeat: return "Repeat";
    case S8OpKind::ReduceOr: return "ReduceOr";
    case S8OpKind::ReduceAnd: return "ReduceAnd";
    case S8OpKind::ReduceXor: return "ReduceXor";
    }
    return "Op";
}

std::string stmtText(const S8NormCFG& fn, const S8Stmt& stmt) {
    std::ostringstream os;
    switch (stmt.kind) {
    case S8StmtKind::Assign:
        os << "assign " << symbolName(fn, stmt.target) << " = "
           << operandText(fn, stmt.value);
        break;
    case S8StmtKind::Op:
        os << "op " << symbolName(fn, stmt.target) << " = "
           << opName(stmt.op.kind) << "<" << stmt.op.result_width << ">(";
        for (std::size_t i = 0; i < stmt.op.operands.size(); ++i) {
            if (i) os << ", ";
            os << operandText(fn, stmt.op.operands[i]);
        }
        os << ")";
        if (stmt.op.hi >= 0 || stmt.op.lo >= 0 || stmt.op.bit >= 0 || stmt.op.times > 0) {
            os << " meta{hi=" << stmt.op.hi << ",lo=" << stmt.op.lo
               << ",bit=" << stmt.op.bit << ",times=" << stmt.op.times << "}";
        }
        break;
    case S8StmtKind::Lookup:
        os << "lookup " << symbolName(fn, stmt.target) << " = lookup("
           << operandText(fn, stmt.lookup_index);
        for (const auto& elem : stmt.lookup_elements) os << ", " << operandText(fn, elem);
        os << ")";
        break;
    case S8StmtKind::LookupWrite:
        os << "lookupwrite [";
        for (std::size_t i = 0; i < stmt.lookup_write_targets.size(); ++i) {
            if (i) os << ", ";
            os << symbolName(fn, stmt.lookup_write_targets[i]);
        }
        os << "] = lookupwrite(" << operandText(fn, stmt.lookup_index)
           << ", " << operandText(fn, stmt.lookup_value);
        for (const auto& elem : stmt.lookup_elements) os << ", " << operandText(fn, elem);
        os << ")";
        break;
    }
    return os.str();
}

std::string termName(S8TermKind kind) {
    switch (kind) {
    case S8TermKind::Jump: return "jump";
    case S8TermKind::Branch: return "branch";
    case S8TermKind::Switch: return "switch";
    case S8TermKind::Exit: return "exit";
    case S8TermKind::Unreachable: return "unreachable";
    }
    return "term";
}

} // namespace

void verifyNormProgram(const S8NormProgram& program) {
    const auto& fn = program.top;
    for (std::size_t i = 0; i < fn.symbols.size(); ++i) {
        if (fn.symbols[i].id != static_cast<SymbolId>(i)) fail("S8 symbol ids must be dense");
        if (fn.symbols[i].type.width <= 0) fail("S8 symbol has invalid width");
        if (fn.symbols[i].type.kind == S8TypeKind::Bool && fn.symbols[i].type.width != 1) {
            fail("S8 bool symbol must be width 1");
        }
    }
    auto verify_operand = [&](const S8Operand& operand) {
        if (operand.type.width <= 0) fail("S8 operand has invalid width");
        if (operand.kind == S8OperandKind::Var) {
            (void)symbolAt(fn, operand.symbol);
        } else {
            if (operand.literal.valid_width != operand.type.width) {
                fail("S8 literal width does not match operand type");
            }
            if (static_cast<int>(operand.literal.words.size()) != wordCount(operand.literal.valid_width)) {
                fail("S8 literal word count mismatch");
            }
        }
    };
    for (const auto& block : fn.blocks) {
        for (const auto& stmt : block.stmts) {
            switch (stmt.kind) {
            case S8StmtKind::Assign:
                (void)symbolAt(fn, stmt.target);
                verify_operand(stmt.value);
                if (!typeEq(symbolType(fn, stmt.target), stmt.value.type)) {
                    fail("S8 assign value type does not match target type");
                }
                break;
            case S8StmtKind::Op:
                (void)symbolAt(fn, stmt.target);
                if (stmt.op.result_width <= 0) fail("S8 op has invalid result width");
                if (symbolType(fn, stmt.target).width != stmt.op.result_width) {
                    fail("S8 op result width does not match target symbol width");
                }
                for (const auto& operand : stmt.op.operands) verify_operand(operand);
                break;
            case S8StmtKind::Lookup:
                (void)symbolAt(fn, stmt.target);
                verify_operand(stmt.lookup_index);
                for (const auto& operand : stmt.lookup_elements) {
                    verify_operand(operand);
                    if (!typeEq(symbolType(fn, stmt.target), operand.type)) {
                        fail("S8 lookup element type does not match target type");
                    }
                }
                break;
            case S8StmtKind::LookupWrite:
                verify_operand(stmt.lookup_index);
                verify_operand(stmt.lookup_value);
                for (const auto& operand : stmt.lookup_elements) verify_operand(operand);
                for (SymbolId target : stmt.lookup_write_targets) (void)symbolAt(fn, target);
                break;
            }
        }
        if (block.terminator.kind == S8TermKind::Branch) {
            verify_operand(block.terminator.condition);
            if (!isBoolType(block.terminator.condition.type)) {
                fail("S8 branch condition must be bool");
            }
        } else if (block.terminator.kind == S8TermKind::Switch) {
            verify_operand(block.terminator.switch_value);
            for (const auto& target : block.terminator.switch_targets) {
                if (target.value) verify_operand(target.value.value());
            }
        }
    }
}

std::string debugPrint(const S8NormProgram& program,
                       const std::vector<NormSummary>& summaries) {
    std::ostringstream os;
    os << "s8opnorm\n";
    for (const auto& summary : summaries) {
        os << "summary function=" << summary.function_name
           << " inserted_casts=" << summary.inserted_casts
           << " normalized_ops=" << summary.normalized_ops
           << " parsed_literals=" << summary.parsed_literals << "\n";
    }
    const auto& fn = program.top;
    os << "top " << fn.name << " entry=bb" << fn.entry << " exit=bb" << fn.exit << "\n";
    os << "symbols\n";
    for (const auto& symbol : fn.symbols) {
        os << "  %" << symbol.id << " " << symbol.debug_name
           << " " << typeText(symbol.type) << "\n";
    }
    os << "ports\n";
    for (const auto& port : fn.ports) os << "  " << symbolName(fn, port.symbol) << "\n";
    for (const auto& block : fn.blocks) {
        os << "  bb" << block.id << "\n";
        for (const auto& stmt : block.stmts) os << "    " << stmtText(fn, stmt) << "\n";
        os << "    term " << termName(block.terminator.kind);
        if (block.terminator.kind == S8TermKind::Branch) {
            os << " " << operandText(fn, block.terminator.condition)
               << " ? bb" << block.terminator.true_target
               << " : bb" << block.terminator.false_target;
        } else if (block.terminator.kind == S8TermKind::Jump) {
            os << " bb" << block.terminator.jump_target;
        } else if (block.terminator.kind == S8TermKind::Switch) {
            os << " " << operandText(fn, block.terminator.switch_value);
        }
        os << "\n";
    }
    return os.str();
}

NormResult normalizeOperations(const S7FlattenedProgram& program,
                               const NormOptions& options) {
    try {
        NormResult result;
        NormSummary summary;
        S8NormProgram out;
        out.top = normalizeFunction(program.top, options, summary);
        verifyNormProgram(out);
        result.summaries.push_back(summary);
        if (options.debug_print) result.debug_text = debugPrint(out, result.summaries);
        result.program = std::move(out);
        return result;
    } catch (const RTLZZException& ex) {
        NormResult result;
        NormError error;
        error.context = ex.primaryContext().value_or(makeContext());
        error.message = ex.message();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    } catch (const std::exception& ex) {
        NormResult result;
        NormError error;
        error.context = makeContext();
        error.message = ex.what();
        error.formatted = ex.what();
        result.error = std::move(error);
        return result;
    }
}

S8NormProgram normalizeOperationsOrThrow(const S7FlattenedProgram& program,
                                         const NormOptions& options) {
    auto result = normalizeOperations(program, options);
    if (!result.ok()) throw RTLZZException(result.error->context, result.error->message);
    return std::move(result.program.value());
}

} // namespace pred::s8opnorm
