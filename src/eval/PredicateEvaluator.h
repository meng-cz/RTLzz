#pragma once

#include "ast/AST.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred {

struct EvalBits {
    std::vector<std::uint64_t> limbs;
    int width = 0;
    bool is_signed = false;

    std::string hex() const;
};

class PredicateEvaluator {
public:
    void setVar(const std::string& name, EvalBits value);
    void setLookupTable(const std::string& name, std::vector<EvalBits> values);
    EvalBits eval(const ExprPtr& expr) const;

    static EvalBits fromUInt64(std::uint64_t value, int width, bool is_signed = false);
    static EvalBits fromLiteral(const std::string& text, int width, bool is_signed = false);

private:
    static EvalBits mask(EvalBits value);
    static bool bit(const EvalBits& value, int index);
    static void setBit(EvalBits& value, int index, bool bit_value);
    static EvalBits add(const EvalBits& a, const EvalBits& b, int width);
    static EvalBits sub(const EvalBits& a, const EvalBits& b, int width);
    static EvalBits mul(const EvalBits& a, const EvalBits& b, int width, bool signed_semantics);
    static EvalBits bitwise(const EvalBits& a, const EvalBits& b, int width, char op);
    static EvalBits shl(const EvalBits& a, int amount, int width);
    static EvalBits lshr(const EvalBits& a, int amount, int width);
    static EvalBits ashr(const EvalBits& a, int amount, int width);
    static EvalBits slice(const EvalBits& a, int hi, int lo);
    static EvalBits writeSlice(EvalBits base, int hi, int lo, const EvalBits& value);
    static EvalBits writeBit(EvalBits base, int index, const EvalBits& value);
    static EvalBits concat(const std::vector<EvalBits>& parts);
    static EvalBits signExtend(EvalBits value, int width);
    static EvalBits twosComplement(EvalBits value);
    static int compareUnsigned(const EvalBits& a, const EvalBits& b);
    static int compareSigned(const EvalBits& a, const EvalBits& b);
    static int shiftAmount(const EvalBits& value);
    static bool signedType(const TypeInfo& type);

    std::unordered_map<std::string, EvalBits> vars_;
    std::unordered_map<std::string, std::vector<EvalBits>> lookup_tables_;
};

} // namespace pred
