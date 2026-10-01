#include <cstdint>
#include <fixint.hpp>

enum class UnsignedMode : uint8_t {
    Low = 0x02,
    Hi = 0x80,
};

enum class SignedMode : int8_t {
    NegTwo = -2,
    PosSeven = 7,
};

enum : uint32_t {
    CONFIG_LOW_BITS = 8,
    CONFIG_MID_LO = 8,
    CONFIG_WORD_BITS = 16,
};

#pragma input_port a
Int<8> a;
#pragma input_port b
Int<8> b;
#pragma input_port word
Int<16> word;
#pragma input_port sh
Int<3> sh;
#pragma input_port dyn
Int<2> dyn;
#pragma input_port wide_a
Int<96> wide_a;
#pragma input_port wide_b
Int<96> wide_b;
#pragma input_port wide_sh
Int<6> wide_sh;
#pragma input_port sel
bool sel;
#pragma input_port signed_shift_lhs
int32_t signed_shift_lhs;
#pragma output_port arith_add
Int<8> arith_add;
#pragma output_port addcarry_bool
Int<9> addcarry_bool;
#pragma output_port addcarry_bit
Int<9> addcarry_bit;
#pragma output_port addcarry_wide
Int<97> addcarry_wide;
#pragma output_port addcarry_no_carry
Int<9> addcarry_no_carry;
#pragma output_port addcarry_one_bit
Int<2> addcarry_one_bit;
#pragma output_port arith_sub_neg
Int<8> arith_sub_neg;
#pragma output_port arith_mul
Int<16> arith_mul;
#pragma output_port mul_signed_left
Int<16> mul_signed_left;
#pragma output_port mul_signed_pair
Int<16> mul_signed_pair;
#pragma output_port mul_signed_wide
Int<24> mul_signed_wide;
#pragma output_port mul_signed_truncated
Int<8> mul_signed_truncated;
#pragma output_port bit_logic
Int<8> bit_logic;
#pragma output_port bit_not_mix
Int<8> bit_not_mix;
#pragma output_port shift_logical
Int<8> shift_logical;
#pragma output_port shift_signed
Int<8> shift_signed;
#pragma output_port signed_shift_result
Int<32> signed_shift_result;
#pragma output_port cmp_unsigned
bool cmp_unsigned;
#pragma output_port cmp_signed
bool cmp_signed;
#pragma output_port cmp_eq_mix
bool cmp_eq_mix;
#pragma output_port range_static
Int<16> range_static;
#pragma output_port range_bit
bool range_bit;
#pragma output_port range_dynamic
Int<16> range_dynamic;
#pragma output_port range_symbolic_at
Int<8> range_symbolic_at;
#pragma output_port range_config_at
Int<8> range_config_at;
#pragma output_port cat_repeat
Int<24> cat_repeat;
#pragma output_port reduce_any
bool reduce_any;
#pragma output_port reduce_all
bool reduce_all;
#pragma output_port reduce_parity
bool reduce_parity;
#pragma output_port cast_unsigned
Int<16> cast_unsigned;
#pragma output_port cast_signed
Int<16> cast_signed;
#pragma output_port cast_trunc
Int<8> cast_trunc;
#pragma output_port enum_unsigned_value
Int<8> enum_unsigned_value;
#pragma output_port enum_signed_value
Int<8> enum_signed_value;
#pragma output_port enum_unsigned_cmp
bool enum_unsigned_cmp;
#pragma output_port enum_signed_cmp
bool enum_signed_cmp;
#pragma output_port enum_signed_ext
Int<16> enum_signed_ext;
#pragma output_port stdmix_add_u8
Int<16> stdmix_add_u8;
#pragma output_port stdmix_mul_u8
Int<16> stdmix_mul_u8;
#pragma output_port stdmix_mul_s8
Int<16> stdmix_mul_s8;
#pragma output_port stdmix_mul_sint_s8
Int<16> stdmix_mul_sint_s8;
#pragma output_port stdmix_cmp_u8
bool stdmix_cmp_u8;
#pragma output_port stdmix_cmp_s8
bool stdmix_cmp_s8;
#pragma output_port stdmix_assign_u8
Int<16> stdmix_assign_u8;
#pragma output_port stdmix_assign_s8
Int<16> stdmix_assign_s8;
#pragma output_port std_to_plain_u32
Int<32> std_to_plain_u32;
#pragma output_port std_to_template_u32
Int<32> std_to_template_u32;
#pragma output_port std_to_u32_equal
bool std_to_u32_equal;
#pragma output_port wide_add
Int<97> wide_add;
#pragma output_port wide_sub
Int<96> wide_sub;
#pragma output_port wide_mul
Int<192> wide_mul;
#pragma output_port wide_bit_mix
Int<96> wide_bit_mix;
#pragma output_port wide_shift_logical
Int<96> wide_shift_logical;
#pragma output_port wide_shift_signed
Int<96> wide_shift_signed;
#pragma output_port wide_cmp_unsigned
bool wide_cmp_unsigned;
#pragma output_port wide_cmp_signed
bool wide_cmp_signed;
#pragma output_port wide_cat
Int<128> wide_cat;
#pragma output_port wide_repeat
Int<128> wide_repeat;
#pragma output_port wide_ext_mix
Int<128> wide_ext_mix;
#pragma output_port wide_dynamic
Int<96> wide_dynamic;
#pragma output_port wide_reduce_any
bool wide_reduce_any;
#pragma output_port wide_reduce_all
bool wide_reduce_all;
#pragma output_port wide_reduce_parity
bool wide_reduce_parity;

// Dynamic write and loop-unrolled slice regression inputs/outputs.
#pragma input_port pick_data
Int<128> pick_data;
#pragma input_port pick_bit_index
Int<7> pick_bit_index;
#pragma input_port pick_bit_value
Int<1> pick_bit_value;
#pragma output_port pick_constant
Int<128> pick_constant;
#pragma output_port pick_static
Int<128> pick_static;
#pragma output_port pick_dynamic
Int<128> pick_dynamic;
#pragma output_port pick_bit
Int<128> pick_bit;
#pragma output_port pick_ordered
Int<128> pick_ordered;
#pragma output_port pick_overlap
Int<128> pick_overlap;
#pragma output_port pick_edges
Int<128> pick_edges;
#pragma output_port pick_full
Int<128> pick_full;

void test_pick_writes() {
    // Exact constant-loop regression; all four writes should fold to one literal.
    Int<128> x = 0;
    for (int i = 0; i < 4; i++) x.pick<32>(i * 32) = Int<32>(i);
    pick_constant = x;

    // Runtime data keeps the static read/write slice path observable.
    Int<128> slices = pick_data;
    for (int i = 0; i < 4; ++i)
        slices.pick<32>(i * 32) = Int<32>(pick_data.pick<32>(i * 32) + Int<32>(i));
    pick_static = slices;

    Int<128> dynamic = pick_data;
    dynamic.pick<32>(dyn.to<unsigned>() * 32) = Int<32>(7);
    pick_dynamic = dynamic;
    Int<128> bits = pick_data;
    bits.pick(pick_bit_index.to<unsigned>()) = pick_bit_value;
    pick_bit = bits;

    // Assignment evaluates RHS before the LHS index expression.
    unsigned pick_i = 1;
    Int<128> ordered = 0;
    ordered.pick<32>((pick_i++) * 32) = Int<32>(pick_i++);
    pick_ordered = ordered;

    Int<128> overlap = pick_data;
    for (int i = 0; i < 4; ++i) overlap.pick<32>(i * 4) = Int<32>(i);
    pick_overlap = overlap;
    Int<128> edges = pick_data;
    edges.pick<32>(0) = Int<32>(7);
    edges.pick<32>(96) = Int<32>(11);
    pick_edges = edges;
    edges.pick<128>(0) = pick_data;
    pick_full = edges;
}

int32_t signed_shift_identity(int32_t value) {
    return value;
}

// Integrated regression from bit_update_coalescing.logic.cpp
#pragma input_port bit_updates_base
Int<32> bit_updates_base;
#pragma input_port bit_updates_a
Int<8> bit_updates_a;
#pragma input_port bit_updates_b
Int<8> bit_updates_b;
#pragma input_port bit_updates_c
Int<8> bit_updates_c;
#pragma output_port bit_updates_adjacent
Int<32> bit_updates_adjacent;
#pragma output_port bit_updates_overlap
Int<32> bit_updates_overlap;
#pragma output_port bit_updates_sparse
Int<32> bit_updates_sparse;

// Integrated regression from width_truncation.logic.cpp
struct NarrowFields {
    Int<5> five;
    Int<17> seventeen;
};

#pragma input_port width_wide
Int<32> width_wide;
#pragma input_port width_shift
Int<6> width_shift;

#pragma output_port width_mask4
Int<4> width_mask4;
#pragma output_port width_trunc5
Int<5> width_trunc5;
#pragma output_port width_trunc17
Int<17> width_trunc17;
#pragma output_port width_left5
Int<5> width_left5;
#pragma output_port width_logical17
Int<17> width_logical17;
#pragma output_port width_arithmetic17
Int<17> width_arithmetic17;
#pragma output_port width_constant_left5
Int<5> width_constant_left5;
#pragma output_port width_constant_arithmetic17
Int<17> width_constant_arithmetic17;
#pragma output_port width_signed_trunc5
Int<5> width_signed_trunc5;
#pragma output_port width_field5
Int<5> width_field5;
#pragma output_port width_field17
Int<17> width_field17;

// Integrated regression from narrowed_shift_slice.logic.cpp
#pragma input_port width_narrowed_pc
Int<64> width_narrowed_pc;
#pragma output_port width_narrowed_index
Int<6> width_narrowed_index;
#pragma output_port width_narrowed_hit
bool width_narrowed_hit;
#pragma output_port width_narrowed_tag
Int<56> width_narrowed_tag;

// Integrated regression from signed_narrow_facts.logic.cpp
#pragma input_port width_signed_narrow_wide
Int<128> width_signed_narrow_wide;
#pragma output_port width_signed_narrow_sign
Int<1> width_signed_narrow_sign;
#pragma output_port width_signed_narrow_narrow
Int<65> width_signed_narrow_narrow;
#pragma output_port width_signed_narrow_positive_sign
Int<1> width_signed_narrow_positive_sign;
#pragma output_port width_signed_narrow_negative_constant
Int<7> width_signed_narrow_negative_constant;
#pragma output_port width_signed_narrow_zero_constant
Int<1> width_signed_narrow_zero_constant;

// Integrated regression from widened_shift.logic.cpp
#pragma input_port width_widened_value
Int<32> width_widened_value;
#pragma input_port width_widened_index
Int<3> width_widened_index;
#pragma input_port width_widened_amount
Int<8> width_widened_amount;
#pragma output_port width_widened_shifted
Int<128> width_widened_shifted;
#pragma output_port width_widened_shifted_constant
Int<8> width_widened_shifted_constant;
#pragma output_port width_widened_shifted_small
Int<4> width_widened_shifted_small;
#pragma output_port width_widened_variable_wide
Int<128> width_widened_variable_wide;
#pragma output_port width_widened_wide_arithmetic
Int<128> width_widened_wide_arithmetic;
#pragma output_port width_widened_narrow_arithmetic
Int<16> width_widened_narrow_arithmetic;
#pragma output_port width_widened_variable_small
Int<4> width_widened_variable_small;

void hls_main() {
    constexpr int LOW_BITS = 8;
    constexpr int MID_LO = 8;
    constexpr int WORD_BITS = 16;

    Int<8> low = Int<8>(word.at<7, 0>());
    Int<8> high = Int<8>(word.at<15, 8>());

    arith_add = a + b;
    addcarry_bool = AddCarry(a, b, sel);
    addcarry_bit = AddCarry<8>(a, b, Int<1>(sel));
    addcarry_wide = AddCarry(wide_a, wide_b, Int<1>(sel));
    addcarry_no_carry = AddCarry<8>(a, b);
    addcarry_one_bit = AddCarry(a.at<0>(), b.at<0>(), sel);
    arith_sub_neg = -(a - b);
    arith_mul = a * b;

    bit_logic = (a & b) | (low ^ high);
    bit_not_mix = ~a;

    shift_logical = a >> sh;
    shift_signed = a.sint() >> sh;
    int32_t signed_value = signed_shift_identity(signed_shift_lhs);
    signed_shift_result = Int<32>((signed_value >> 1) - 32);

    cmp_unsigned = a > b;
    cmp_signed = a.sint() < b.sint();
    cmp_eq_mix = (a == Int<8>(word.at<7, 0>())) != sel;
    Int<16> patched = word;
    patched.at<7, 0>() = b;
    range_static = patched;
    range_bit = Int<1>(word.at<3, 3>()) != Int<1>(0);

    Int<4> dyn_read = a.pick<4>(dyn);
    range_dynamic = Int<16>(dyn_read);
    Int<8> symbolic_low = Int<8>(word.at<LOW_BITS - 1, 0>());
    Int<8> symbolic_high = Int<8>(word.at<WORD_BITS - 1, MID_LO>());
    range_symbolic_at = symbolic_low ^ symbolic_high;
    Int<8> config_low = Int<8>(word.at<CONFIG_LOW_BITS - 1, 0>());
    Int<8> config_high = Int<8>(word.at<CONFIG_WORD_BITS - 1, CONFIG_MID_LO>());
    range_config_at = config_low ^ config_high;

    Int<12> joined = Cat(a, Int<4>(b.at<3, 0>()));
    cat_repeat = Repeat<2>(joined);
    reduce_any = ReduceOr(joined);
    reduce_all = ReduceAnd(joined);
    reduce_parity = ReduceXor(joined);
    cast_unsigned = Int<16>(a);
    cast_signed = Int<16>(a.sint());
    cast_trunc = Int<8>(word);

    UnsignedMode unsigned_mode = sel ? UnsignedMode::Hi : UnsignedMode::Low;
    SignedMode signed_mode = sel ? SignedMode::NegTwo : SignedMode::PosSeven;
    enum_unsigned_value = static_cast<uint8_t>(unsigned_mode);
    enum_signed_value = static_cast<int8_t>(signed_mode);
    enum_unsigned_cmp =
        static_cast<uint8_t>(UnsignedMode::Hi) >
        static_cast<uint8_t>(UnsignedMode::Low);
    enum_signed_cmp =
        static_cast<int8_t>(SignedMode::NegTwo) <
        static_cast<int8_t>(SignedMode::PosSeven);
    enum_signed_ext = static_cast<int8_t>(signed_mode);

    uint8_t std_u8 = a.template to<uint8_t>();
    int8_t std_s8 = b.template to<int8_t>();
    stdmix_add_u8 = a + std_u8;
    stdmix_mul_u8 = a * std_u8;
    stdmix_mul_s8 = a * std_s8;
    stdmix_mul_sint_s8 = a.sint() * std_s8;
    mul_signed_left = std_s8 * a;
    mul_signed_pair = a.sint() * b.sint();
    mul_signed_wide = word * std_s8;
    mul_signed_truncated = Int<8>(a * std_s8);
    stdmix_cmp_u8 = a > std_u8;
    stdmix_cmp_s8 = b.sint() < std_s8;
    stdmix_assign_u8 = std_u8;
    stdmix_assign_s8 = std_s8;
    Int<32> std_to_src = Cat(word, word);
    uint32_t std_u32_plain = std_to_src.to<uint32_t>();
    uint32_t std_u32_template = std_to_src.template to<uint32_t>();
    std_to_plain_u32 = std_u32_plain;
    std_to_template_u32 = std_u32_template;
    std_to_u32_equal = std_u32_plain == std_u32_template;

    Int<96> wide_mix = wide_a ^ wide_b;
    Int<128> wide_ext = Int<128>(wide_a);
    Int<80> wide_low80 = Int<80>(wide_a.at<79, 0>());
    Int<48> wide_high48 = Int<48>(wide_b.at<95, 48>());
    Int<64> wide_low64 = Int<64>(wide_mix.at<63, 0>());
    Int<16> wide_dyn16 = wide_a.pick<16>(dyn);

    wide_add = wide_a + wide_b;
    wide_sub = wide_a - wide_b;
    wide_mul = wide_a * wide_b;
    wide_bit_mix = (wide_a & wide_b) | (~wide_b);
    wide_shift_logical = wide_a >> wide_sh;
    wide_shift_signed = wide_a.sint() >> wide_sh;
    wide_cmp_unsigned = wide_a > wide_b;
    wide_cmp_signed = wide_a.sint() < wide_b.sint();
    wide_cat = Cat(wide_low80, wide_high48);
    wide_repeat = Repeat<2>(wide_low64);
    wide_ext_mix = wide_ext ^ wide_cat;
    wide_dynamic = Int<96>(wide_dyn16);
    wide_reduce_any = ReduceOr(wide_a);
    wide_reduce_all = ReduceAnd(wide_b);
    wide_reduce_parity = ReduceXor(wide_mix);

    test_pick_writes();

    // bit_update_coalescing.logic.cpp
    {
    Int<32> value = bit_updates_base;
    value.at<7, 0>() = bit_updates_a;
    value.at<15, 8>() = bit_updates_b;
    value.at<23, 16>() = bit_updates_c;
    bit_updates_adjacent = value;

    value = bit_updates_base;
    value.pick<12>(0) = Int<12>(bit_updates_a);
    value.pick<12>(4) = Int<12>(bit_updates_b);
    bit_updates_overlap = value;

    value = bit_updates_base;
    value.at<7, 0>() = bit_updates_a;
    value.at<31, 24>() = bit_updates_c;
    bit_updates_sparse = value;
    }

    // width_truncation.logic.cpp
    {
    uint32_t address = width_wide.template to<uint32_t>();
    width_mask4 = Int<4>(1U << (address & 3U));

    width_trunc5 = Int<5>(width_wide);
    width_trunc17 = Int<17>(width_wide);
    width_left5 = Int<5>(width_wide << width_shift);
    width_logical17 = Int<17>(width_wide >> width_shift);
    width_arithmetic17 = Int<17>(width_wide.sint() >> width_shift);
    width_constant_left5 = Int<5>(width_wide << 32);
    width_constant_arithmetic17 = Int<17>(width_wide.sint() >> 32);
    width_signed_trunc5 = Int<5>(width_wide.sint());

    NarrowFields fields;
    fields.five = Int<5>(width_wide << width_shift);
    fields.seventeen = Int<17>(width_wide.sint() >> width_shift);
    width_field5 = fields.five;
    width_field17 = fields.seventeen;

    // narrowed_shift_slice.logic.cpp
    {
    width_narrowed_index = Int<6>(width_narrowed_pc >> 2U);
    width_narrowed_hit = Int<6>(width_narrowed_pc >> 2U) == Int<6>(17);
    width_narrowed_tag = Int<56>(width_narrowed_pc >> 8U);
    }

    // signed_narrow_facts.logic.cpp
    {
    width_signed_narrow_sign = Int<1>(width_signed_narrow_wide.sint());
    width_signed_narrow_narrow = Int<65>(width_signed_narrow_wide.sint());
    Int<128> positive = width_signed_narrow_wide & Int<128>(Int<64>(~Int<64>(0)));
    width_signed_narrow_positive_sign = Int<1>(positive.sint());
    width_signed_narrow_negative_constant = Int<7>(Int<32>(-128).sint());
    width_signed_narrow_zero_constant = Int<1>(Int<32>(0).sint());
    }

    // widened_shift.logic.cpp
    {
    width_widened_shifted = Int<128>(width_widened_value) << (width_widened_index.to<uint32_t>() * 16U);
    width_widened_shifted_constant = Int<8>(width_widened_index) << 4U;
    width_widened_shifted_small = Int<4>(width_widened_index << 4U);
    width_widened_variable_wide = Int<128>(width_widened_value) << width_widened_amount;
    Int<128> sign_extended = Int<128>(width_widened_value.sint());
    width_widened_wide_arithmetic = sign_extended.sint() >> width_widened_amount;
    width_widened_narrow_arithmetic = Int<16>(sign_extended.sint() >> width_widened_amount);
    width_widened_variable_small = Int<4>(width_widened_index << width_widened_amount);
    }
    }
}
