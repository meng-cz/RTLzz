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

void hls_main(Int<8> a,
              Int<8> b,
              Int<16> word,
              Int<3> sh,
              Int<2> dyn,
              bool sel,
              Int<9>& arith_add,
              Int<8>& arith_sub_neg,
              Int<16>& arith_mul,
              Int<8>& bit_logic,
              Int<8>& bit_not_mix,
              Int<8>& shift_logical,
              Int<8>& shift_signed,
              bool& cmp_unsigned,
              bool& cmp_signed,
              bool& cmp_eq_mix,
              Int<16>& range_static,
              bool& range_bit,
              Int<16>& range_dynamic,
              Int<24>& cat_repeat,
              bool& reduce_any,
              bool& reduce_all,
              bool& reduce_parity,
              Int<16>& cast_unsigned,
              Int<16>& cast_signed,
              Int<8>& cast_trunc,
              Int<8>& enum_unsigned_value,
              Int<8>& enum_signed_value,
              bool& enum_unsigned_cmp,
              bool& enum_signed_cmp,
              Int<16>& enum_signed_ext,
              Int<16>& stdmix_add_u8,
              Int<16>& stdmix_mul_u8,
              Int<16>& stdmix_mul_s8,
              Int<16>& stdmix_mul_sint_s8,
              bool& stdmix_cmp_u8,
              bool& stdmix_cmp_s8,
              Int<16>& stdmix_assign_u8,
              Int<16>& stdmix_assign_s8) {
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
}
