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
#pragma output_port arith_add
Int<9> arith_add;
#pragma output_port arith_sub_neg
Int<8> arith_sub_neg;
#pragma output_port arith_mul
Int<16> arith_mul;
#pragma output_port bit_logic
Int<8> bit_logic;
#pragma output_port bit_not_mix
Int<8> bit_not_mix;
#pragma output_port shift_logical
Int<8> shift_logical;
#pragma output_port shift_signed
Int<8> shift_signed;
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

void hls_main() {
    Int<8> low = Int<8>(word.at<7, 0>());
    Int<8> high = Int<8>(word.at<15, 8>());

    arith_add = a + b;
    arith_sub_neg = -(a - b);
    arith_mul = a * b;

    bit_logic = (a & b) | (low ^ high);
    bit_not_mix = ~a;

    shift_logical = a >> sh;
    shift_signed = a.sint() >> sh;

    cmp_unsigned = a > b;
    cmp_signed = a.sint() < b.sint();
    cmp_eq_mix = (a == Int<8>(word.at<7, 0>())) != sel;
    Int<16> patched = word;
    patched.at<7, 0>() = b;
    range_static = patched;
    range_bit = Int<1>(word.at<3, 3>()) != Int<1>(0);

    Int<4> dyn_read = a.pick<4>(dyn);
    range_dynamic = Int<16>(dyn_read);

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
}
