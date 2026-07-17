#include <cstdint>
#include <fixint.hpp>

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
              Int<8>& cast_trunc) {
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
}
