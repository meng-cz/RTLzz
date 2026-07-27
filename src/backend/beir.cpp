#include "backend/beir.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pred::beir {

bool DebugInfo::hasSourceLoc() const {
    if (decl_loc && decl_loc->valid()) return true;
    for (const auto& loc : primary_locs) {
        if (loc.valid()) return true;
    }
    for (const auto& loc : related_locs) {
        if (loc.valid()) return true;
    }
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

static bool addUniqueLoc(std::vector<DebugLoc>& locs,
                         const DebugLoc& loc,
                         std::size_t limit) {
    if (!loc.valid() || locs.size() >= limit) return false;
    for (const auto& existing : locs) {
        if (sameDebugLoc(existing, loc)) return false;
    }
    locs.push_back(loc);
    return true;
}

static void addLegacyLoc(DebugInfo& debug, const DebugLoc& loc) {
    addUniqueLoc(debug.source_locs, loc, 32);
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
    addPrimaryDebugLoc(debug, loc);
}

void addPrimaryDebugLoc(DebugInfo& debug, const DebugLoc& loc) {
    if (addUniqueLoc(debug.primary_locs, loc, 4)) addLegacyLoc(debug, loc);
}

void addRelatedDebugLoc(DebugInfo& debug, const DebugLoc& loc) {
    if (addUniqueLoc(debug.related_locs, loc, 12)) addLegacyLoc(debug, loc);
}

void setDeclDebugLoc(DebugInfo& debug, const DebugLoc& loc) {
    if (!loc.valid()) return;
    if (!debug.decl_loc || !debug.decl_loc->valid()) {
        debug.decl_loc = loc;
    }
    addLegacyLoc(debug, loc);
}

void addDebugLocs(DebugInfo& debug, const std::vector<DebugLoc>& locs) {
    for (const auto& loc : locs) addDebugLoc(debug, loc);
}

void addDebugInfoLocs(DebugInfo& debug, const DebugInfo& source) {
    if (source.decl_loc) setDeclDebugLoc(debug, *source.decl_loc);
    for (const auto& loc : source.primary_locs) addPrimaryDebugLoc(debug, loc);
    for (const auto& loc : source.related_locs) addRelatedDebugLoc(debug, loc);
    if (source.primary_locs.empty() && source.related_locs.empty() && !source.decl_loc) {
        addDebugLocs(debug, source.source_locs);
    }
    for (const auto& message : messageStack(source)) addDebugMessage(debug, message);
}

void addDebugInfoAsRelated(DebugInfo& debug, const DebugInfo& source) {
    if (source.decl_loc) addRelatedDebugLoc(debug, *source.decl_loc);
    for (const auto& loc : source.primary_locs) addRelatedDebugLoc(debug, loc);
    for (const auto& loc : source.related_locs) addRelatedDebugLoc(debug, loc);
    if (source.primary_locs.empty() && source.related_locs.empty() && !source.decl_loc) {
        for (const auto& loc : source.source_locs) addRelatedDebugLoc(debug, loc);
    }
    for (const auto& message : messageStack(source)) addDebugMessage(debug, message);
}

void addDebugMessage(DebugInfo& debug, const std::string& message) {
    if (message.empty() || debug.messages.size() >= 64) return;
    for (const auto& existing : debug.messages) {
        if (existing == message) return;
    }
    debug.messages.push_back(message);
    if (debug.reason.empty()) debug.reason = message;
}

void addDebugDerivedOperand(DebugInfo& debug, const Operand& operand) {
    if (operand.kind == OperandKind::Symbol && operand.node != kInvalidNodeId) {
        for (NodeId existing : debug.derived_nodes) {
            if (existing == operand.node) return;
        }
        debug.derived_nodes.push_back(operand.node);
        return;
    }
    if (operand.text.empty()) return;
    for (const auto& existing : debug.derived_names) {
        if (existing == operand.text) return;
    }
    debug.derived_names.push_back(operand.text);
}

void addDebugDerivedOperands(DebugInfo& debug, const std::vector<Operand>& operands) {
    for (const auto& operand : operands) addDebugDerivedOperand(debug, operand);
}

void addOperandDebugLocs(DebugInfo& debug, const Program& program, const Operand& operand) {
    if (operand.kind != OperandKind::Symbol || operand.node == kInvalidNodeId) return;
    const Signal* signal = program.findSignal(operand.node);
    if (!signal) return;
    addDebugInfoAsRelated(debug, signal->debug);
    if (signal->driver) {
        addDebugInfoAsRelated(debug, signal->driver->debug);
        for (const auto& loc : signal->driver->source_locs) addRelatedDebugLoc(debug, loc);
    }
    std::string base_name;
    if (splitVersionedName(signal->name, base_name)) {
        for (const auto& candidate : program.signals) {
            if (candidate.name != base_name) continue;
            addDebugInfoAsRelated(debug, candidate.debug);
            if (candidate.driver) {
                addDebugInfoAsRelated(debug, candidate.driver->debug);
                for (const auto& loc : candidate.driver->source_locs) addRelatedDebugLoc(debug, loc);
            }
            break;
        }
    }
}

void addOperandDebugLocs(DebugInfo& debug, const Program& program, const std::vector<Operand>& operands) {
    for (const auto& operand : operands) addOperandDebugLocs(debug, program, operand);
}

void addOperandReplacementDebug(DebugInfo& debug,
                                const Program& program,
                                const Operand& before,
                                const Operand& after,
                                const std::string& message) {
    debug.origin = DebugOrigin::Generated;
    addDebugMessage(debug, message);
    debug.reason = message;
    addDebugDerivedOperand(debug, before);
    addDebugDerivedOperand(debug, after);
    addOperandDebugLocs(debug, program, before);
    addOperandDebugLocs(debug, program, after);
}

std::vector<DebugLoc> flattenedDebugLocs(const DebugInfo& debug) {
    std::vector<DebugLoc> out;
    auto add = [&](const DebugLoc& loc) {
        addUniqueLoc(out, loc, 32);
    };
    for (const auto& loc : debug.primary_locs) add(loc);
    if (debug.decl_loc) add(*debug.decl_loc);
    for (const auto& loc : debug.related_locs) add(loc);
    for (const auto& loc : debug.source_locs) add(loc);
    return out;
}

std::vector<std::string> messageStack(const DebugInfo& debug) {
    std::vector<std::string> out = debug.messages;
    if (!debug.reason.empty()) {
        bool seen = false;
        for (const auto& message : out) {
            if (message == debug.reason) {
                seen = true;
                break;
            }
        }
        if (!seen) out.push_back(debug.reason);
    }
    return out;
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

static bool factsAllBitsKnown(const ValueFacts& facts) {
    if (!facts.valid || facts.width <= 0) return false;
    for (int bit = 0; bit < facts.width; ++bit) {
        if (!factsGetBit(facts.known_zero, bit) && !factsGetBit(facts.known_one, bit)) return false;
    }
    return true;
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

static bool factsAnyBitSet(const std::vector<std::uint64_t>& limbs, int width) {
    for (int bit = 0; bit < width; ++bit) {
        if (factsGetBit(limbs, bit)) return true;
    }
    return false;
}

static std::vector<std::uint64_t> factsResizeBits(std::vector<std::uint64_t> limbs,
                                                  int old_width,
                                                  int new_width,
                                                  bool sign_extend) {
    factsTrim(limbs, old_width);
    if (new_width <= old_width) {
        factsTrim(limbs, new_width);
        return limbs;
    }
    bool sign = sign_extend && old_width > 0 && factsGetBit(limbs, old_width - 1);
    limbs.resize(factsLimbCount(new_width), sign ? std::numeric_limits<std::uint64_t>::max() : 0);
    if (sign) {
        for (int bit = old_width; bit < new_width; ++bit) factsSetBit(limbs, bit);
    }
    factsTrim(limbs, new_width);
    return limbs;
}

static int factsCompareUnsigned(std::vector<std::uint64_t> lhs,
                                std::vector<std::uint64_t> rhs,
                                int width) {
    factsTrim(lhs, width);
    factsTrim(rhs, width);
    for (std::size_t i = factsLimbCount(width); i > 0; --i) {
        std::uint64_t l = i - 1 < lhs.size() ? lhs[i - 1] : 0;
        std::uint64_t r = i - 1 < rhs.size() ? rhs[i - 1] : 0;
        if (l < r) return -1;
        if (l > r) return 1;
    }
    return 0;
}

static int factsCompareSigned(const std::vector<std::uint64_t>& lhs,
                              const std::vector<std::uint64_t>& rhs,
                              int width) {
    bool lhs_negative = factsGetBit(lhs, width - 1);
    bool rhs_negative = factsGetBit(rhs, width - 1);
    if (lhs_negative != rhs_negative) return lhs_negative ? -1 : 1;
    return factsCompareUnsigned(lhs, rhs, width);
}

static std::vector<std::uint64_t> factsAddBits(const std::vector<std::uint64_t>& lhs,
                                               const std::vector<std::uint64_t>& rhs,
                                               int width) {
    std::vector<std::uint64_t> out = factsZeros(width);
    unsigned __int128 carry = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        unsigned __int128 sum = carry;
        if (i < lhs.size()) sum += lhs[i];
        if (i < rhs.size()) sum += rhs[i];
        out[i] = static_cast<std::uint64_t>(sum);
        carry = sum >> 64;
    }
    factsTrim(out, width);
    return out;
}

static std::vector<std::uint64_t> factsSubBits(const std::vector<std::uint64_t>& lhs,
                                               const std::vector<std::uint64_t>& rhs,
                                               int width) {
    std::vector<std::uint64_t> out = factsZeros(width);
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint64_t l = i < lhs.size() ? lhs[i] : 0;
        std::uint64_t r = i < rhs.size() ? rhs[i] : 0;
        std::uint64_t subtrahend = r + borrow;
        std::uint64_t next_borrow = (l < r || (borrow && subtrahend == 0)) ? 1 : 0;
        out[i] = l - subtrahend;
        borrow = next_borrow;
    }
    factsTrim(out, width);
    return out;
}

static std::vector<std::uint64_t> factsShlBits(const std::vector<std::uint64_t>& value,
                                               int width,
                                               std::uint64_t amount) {
    std::vector<std::uint64_t> out = factsZeros(width);
    if (amount >= static_cast<std::uint64_t>(std::max(width, 0))) return out;
    for (int bit = 0; bit < width; ++bit) {
        std::uint64_t target = static_cast<std::uint64_t>(bit) + amount;
        if (target < static_cast<std::uint64_t>(width) && factsGetBit(value, bit)) {
            factsSetBit(out, static_cast<int>(target));
        }
    }
    return out;
}

static std::vector<std::uint64_t> factsShrBits(const std::vector<std::uint64_t>& value,
                                               int width,
                                               std::uint64_t amount,
                                               bool sign_extend) {
    std::vector<std::uint64_t> out = factsZeros(width);
    bool sign = sign_extend && width > 0 && factsGetBit(value, width - 1);
    for (int bit = 0; bit < width; ++bit) {
        std::uint64_t source = static_cast<std::uint64_t>(bit) + amount;
        if (source < static_cast<std::uint64_t>(width)) {
            if (factsGetBit(value, static_cast<int>(source))) factsSetBit(out, bit);
        } else if (sign) {
            factsSetBit(out, bit);
        }
    }
    return out;
}

static std::vector<std::uint64_t> factsMulBits(const std::vector<std::uint64_t>& lhs,
                                               const std::vector<std::uint64_t>& rhs,
                                               int width) {
    std::vector<std::uint64_t> out = factsZeros(width);
    for (int bit = 0; bit < width; ++bit) {
        if (!factsGetBit(rhs, bit)) continue;
        out = factsAddBits(out, factsShlBits(lhs, width, static_cast<std::uint64_t>(bit)), width);
    }
    return out;
}

static std::optional<std::pair<std::vector<std::uint64_t>, std::vector<std::uint64_t>>>
factsDivModUnsigned(const std::vector<std::uint64_t>& numerator,
                    const std::vector<std::uint64_t>& denominator,
                    int width) {
    if (!factsAnyBitSet(denominator, width)) return std::nullopt;
    std::vector<std::uint64_t> quotient = factsZeros(width);
    std::vector<std::uint64_t> remainder = factsZeros(width);
    for (int bit = width - 1; bit >= 0; --bit) {
        remainder = factsShlBits(remainder, width, 1);
        if (factsGetBit(numerator, bit)) factsSetBit(remainder, 0);
        if (factsCompareUnsigned(remainder, denominator, width) >= 0) {
            remainder = factsSubBits(remainder, denominator, width);
            factsSetBit(quotient, bit);
        }
    }
    return std::make_pair(std::move(quotient), std::move(remainder));
}

static std::optional<std::pair<std::vector<std::uint64_t>, std::vector<std::uint64_t>>>
factsDivModSigned(const std::vector<std::uint64_t>& numerator,
                  const std::vector<std::uint64_t>& denominator,
                  int width) {
    if (!factsAnyBitSet(denominator, width)) return std::nullopt;
    bool numerator_negative = factsGetBit(numerator, width - 1);
    bool denominator_negative = factsGetBit(denominator, width - 1);
    std::vector<std::uint64_t> zero = factsZeros(width);
    std::vector<std::uint64_t> numerator_abs = numerator_negative
        ? factsSubBits(zero, numerator, width)
        : numerator;
    std::vector<std::uint64_t> denominator_abs = denominator_negative
        ? factsSubBits(zero, denominator, width)
        : denominator;
    auto div_mod = factsDivModUnsigned(numerator_abs, denominator_abs, width);
    if (!div_mod) return std::nullopt;
    if (numerator_negative != denominator_negative) {
        div_mod->first = factsSubBits(zero, div_mod->first, width);
    }
    if (numerator_negative) {
        div_mod->second = factsSubBits(zero, div_mod->second, width);
    }
    return div_mod;
}

static bool factsOperandSigned(const Operand& operand, const ValueFacts& facts) {
    return operand.signed_view || operand.constant.signed_view ||
           (facts.constant && facts.value.signed_view);
}

static bool factsConstantToU64(const ValueFacts& facts, std::uint64_t& out) {
    if (!facts.constant || facts.value.width > 64) return false;
    for (std::size_t i = 1; i < facts.value.limbs.size(); ++i) {
        if (facts.value.limbs[i] != 0) return false;
    }
    out = facts.value.limbs.empty() ? 0 : facts.value.limbs[0];
    return true;
}

static void factsPromoteKnownConstant(ValueFacts& facts) {
    if (facts.constant || !factsAllBitsKnown(facts)) return;
    facts.constant = true;
    facts.value = factsMakeConstant(facts.known_one, facts.width);
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

static ValueFacts factsBoolConstant(bool value) {
    return factsFromConstant(factsMakeConstant({value ? 1ULL : 0ULL}, 1), 1);
}

static ValueFacts factsConstantBits(std::vector<std::uint64_t> limbs,
                                    int width,
                                    bool signed_view = false) {
    return factsFromConstant(factsMakeConstant(std::move(limbs), width, signed_view), width);
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

static ValueFacts factsSExt(const ValueFacts& src, int out_width) {
    if (src.width <= 0) return factsUnknown(out_width);
    if (out_width <= src.width) {
        ValueFacts out = factsUnknown(out_width);
        out.known_zero = factsSliceBits(src.known_zero, 0, out_width);
        out.known_one = factsSliceBits(src.known_one, 0, out_width);
        if (src.constant) out = factsFromConstant(factsMakeConstant(src.value.limbs, out_width, src.value.signed_view), out_width);
        return out;
    }

    ValueFacts out = factsUnknown(out_width);
    out.known_zero = factsSliceBits(src.known_zero, 0, src.width);
    out.known_one = factsSliceBits(src.known_one, 0, src.width);
    out.known_zero.resize(factsLimbCount(out_width), 0);
    out.known_one.resize(factsLimbCount(out_width), 0);

    bool sign_zero = factsGetBit(src.known_zero, src.width - 1);
    bool sign_one = factsGetBit(src.known_one, src.width - 1);
    for (int bit = src.width; bit < out_width; ++bit) {
        if (sign_zero) factsSetBit(out.known_zero, bit);
        if (sign_one) factsSetBit(out.known_one, bit);
    }
    factsTrim(out.known_zero, out_width);
    factsTrim(out.known_one, out_width);

    if (src.constant) {
        std::vector<std::uint64_t> limbs = src.value.limbs;
        factsTrim(limbs, out_width);
        if (factsGetBit(src.value.limbs, src.width - 1)) {
            for (int bit = src.width; bit < out_width; ++bit) factsSetBit(limbs, bit);
        }
        out = factsFromConstant(factsMakeConstant(std::move(limbs), out_width, src.value.signed_view), out_width);
    }
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

static std::optional<ValueFacts> factsInferUnaryConstant(const Operation& op,
                                                         const std::vector<ValueFacts>& operands,
                                                         int width) {
    if (operands.empty() || !operands[0].constant) return std::nullopt;
    bool signed_view = factsOperandSigned(op.operands[0], operands[0]);
    std::vector<std::uint64_t> value = factsResizeBits(operands[0].value.limbs,
                                                       operands[0].width,
                                                       width,
                                                       signed_view);
    switch (op.op) {
    case OpCode::LogicNot:
        return factsBoolConstant(!factsAnyBitSet(value, width));
    case OpCode::BitNot:
        return factsConstantBits(factsBitNot(std::move(value), width), width, signed_view);
    case OpCode::Neg:
        return factsConstantBits(factsSubBits(factsZeros(width), value, width),
                                 width,
                                 signed_view);
    default:
        return std::nullopt;
    }
}

static std::optional<ValueFacts> factsInferBinaryConstant(const Operation& op,
                                                          const std::vector<ValueFacts>& operands,
                                                          int width) {
    if (operands.size() < 2 || !operands[0].constant || !operands[1].constant) {
        return std::nullopt;
    }
    bool lhs_signed = factsOperandSigned(op.operands[0], operands[0]);
    bool rhs_signed = factsOperandSigned(op.operands[1], operands[1]);
    bool signed_context = lhs_signed || rhs_signed;

    if (op.op == OpCode::LogicAnd || op.op == OpCode::LogicOr) {
        bool lhs_true = factsAnyBitSet(operands[0].value.limbs, operands[0].width);
        bool rhs_true = factsAnyBitSet(operands[1].value.limbs, operands[1].width);
        return factsBoolConstant(op.op == OpCode::LogicAnd ? (lhs_true && rhs_true)
                                                           : (lhs_true || rhs_true));
    }

    if (op.op == OpCode::Eq || op.op == OpCode::Ne ||
        op.op == OpCode::Lt || op.op == OpCode::Le ||
        op.op == OpCode::Gt || op.op == OpCode::Ge) {
        int compare_width = std::max({1, operands[0].width, operands[1].width});
        std::vector<std::uint64_t> lhs = factsResizeBits(operands[0].value.limbs,
                                                         operands[0].width,
                                                         compare_width,
                                                         lhs_signed);
        std::vector<std::uint64_t> rhs = factsResizeBits(operands[1].value.limbs,
                                                         operands[1].width,
                                                         compare_width,
                                                         rhs_signed);
        int cmp = signed_context ? factsCompareSigned(lhs, rhs, compare_width)
                                 : factsCompareUnsigned(lhs, rhs, compare_width);
        bool result = false;
        switch (op.op) {
        case OpCode::Eq: result = cmp == 0; break;
        case OpCode::Ne: result = cmp != 0; break;
        case OpCode::Lt: result = cmp < 0; break;
        case OpCode::Le: result = cmp <= 0; break;
        case OpCode::Gt: result = cmp > 0; break;
        case OpCode::Ge: result = cmp >= 0; break;
        default: break;
        }
        return factsBoolConstant(result);
    }

    std::vector<std::uint64_t> lhs = factsResizeBits(operands[0].value.limbs,
                                                     operands[0].width,
                                                     width,
                                                     lhs_signed);
    std::vector<std::uint64_t> rhs = factsResizeBits(operands[1].value.limbs,
                                                     operands[1].width,
                                                     width,
                                                     rhs_signed);

    switch (op.op) {
    case OpCode::Add:
        return factsConstantBits(factsAddBits(lhs, rhs, width), width, signed_context);
    case OpCode::Sub:
        return factsConstantBits(factsSubBits(lhs, rhs, width), width, signed_context);
    case OpCode::Mul:
        return factsConstantBits(factsMulBits(lhs, rhs, width), width, signed_context);
    case OpCode::Div:
    case OpCode::Mod: {
        auto div_mod = signed_context
            ? factsDivModSigned(lhs, rhs, width)
            : factsDivModUnsigned(lhs, rhs, width);
        if (!div_mod) return std::nullopt;
        return factsConstantBits(op.op == OpCode::Div ? div_mod->first : div_mod->second,
                                 width,
                                 signed_context);
    }
    case OpCode::BitAnd: {
        std::vector<std::uint64_t> out = factsZeros(width);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = (i < lhs.size() ? lhs[i] : 0) & (i < rhs.size() ? rhs[i] : 0);
        }
        factsTrim(out, width);
        return factsConstantBits(std::move(out), width, signed_context);
    }
    case OpCode::BitOr: {
        std::vector<std::uint64_t> out = factsZeros(width);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = (i < lhs.size() ? lhs[i] : 0) | (i < rhs.size() ? rhs[i] : 0);
        }
        factsTrim(out, width);
        return factsConstantBits(std::move(out), width, signed_context);
    }
    case OpCode::BitXor: {
        std::vector<std::uint64_t> out = factsZeros(width);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = (i < lhs.size() ? lhs[i] : 0) ^ (i < rhs.size() ? rhs[i] : 0);
        }
        factsTrim(out, width);
        return factsConstantBits(std::move(out), width, signed_context);
    }
    case OpCode::Shl:
    case OpCode::Shr: {
        std::uint64_t amount = 0;
        if (!factsConstantToU64(operands[1], amount)) return std::nullopt;
        return factsConstantBits(op.op == OpCode::Shl
                                     ? factsShlBits(lhs, width, amount)
                                     : factsShrBits(lhs, width, amount, lhs_signed),
                                 width,
                                 lhs_signed);
    }
    default:
        return std::nullopt;
    }
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
    if (op.kind == OperationKind::SExt) return operands.empty() ? factsUnknown(width) : factsSExt(operands[0], width);
    if (op.kind == OperationKind::Trunc) return operands.empty() ? factsUnknown(width) : factsTrunc(operands[0], width);
    if (op.kind == OperationKind::Slice) return operands.empty() ? factsUnknown(width) : factsSlice(operands[0], op.lo, width);
    if (op.kind == OperationKind::BitSelect) return operands.empty() ? factsUnknown(1) : factsBitSelect(operands[0], op.bit);
    if (op.kind == OperationKind::Unary) {
        if (auto folded = factsInferUnaryConstant(op, operands, width)) return *folded;
    }
    if (op.kind == OperationKind::Binary) {
        if (auto folded = factsInferBinaryConstant(op, operands, width)) return *folded;
    }
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
            factsPromoteKnownConstant(facts);
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
        bool signal_changed = false;
        for (auto& operand : signal.driver->operands) {
            Operand resolved = resolveOperand(operand, aliases);
            if (!(operandSignature(resolved) == operandSignature(operand))) {
                Operand before = operand;
                addOperandReplacementDebug(signal.driver->debug,
                                           program_,
                                           before,
                                           resolved,
                                           "replaced aliased BEIR operand");
                operand = std::move(resolved);
                signal_changed = true;
                changed = true;
            }
        }
        if (signal_changed) {
            signal.driver->source_locs = signal.driver->debug.source_locs;
            signal.debug = signal.driver->debug;
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

static void emitOneLoc(std::ostream& os, const DebugLoc& loc) {
    os << loc.file << ":" << loc.line << ":" << loc.column;
    if (loc.end_line || loc.end_column) {
        os << "-" << loc.end_line << ":" << loc.end_column;
    }
}

static void emitNamedLocs(std::ostream& os,
                          const char* name,
                          const std::vector<DebugLoc>& locs) {
    if (locs.empty()) return;
    os << " " << name << "=[";
    bool any = false;
    for (const auto& loc : locs) {
        if (!loc.valid()) continue;
        if (any) os << ", ";
        emitOneLoc(os, loc);
        any = true;
    }
    os << "]";
}

static void emitDebugInfo(std::ostream& os, const DebugInfo& debug) {
    os << " debug_origin=" << debugOriginText(debug.origin);
    if (debug.decl_loc && debug.decl_loc->valid()) {
        os << " debug_decl=";
        emitOneLoc(os, *debug.decl_loc);
    }
    emitNamedLocs(os, "debug_primary", debug.primary_locs);
    emitNamedLocs(os, "debug_related", debug.related_locs);
    if (!debug.reason.empty()) os << " debug_reason=\"" << debug.reason << "\"";
    const auto messages = messageStack(debug);
    if (!messages.empty()) {
        os << " debug_messages=[";
        for (std::size_t i = 0; i < messages.size(); ++i) {
            if (i) os << ", ";
            os << "\"" << messages[i] << "\"";
        }
        os << "]";
    }
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

} // namespace pred::beir
