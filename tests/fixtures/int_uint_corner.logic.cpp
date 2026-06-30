#include <fixint.hpp>

void hls_main(Int<8> a,
              Int<8> b,
              Int<3> sh,
              Int<8>& unsigned_shift,
              Int<8>& signed_shift,
              Int<4>& signed_narrow_pos,
              Int<4>& signed_narrow_neg,
              Int<8>& neg_const,
              bool& plain_cmp,
              bool& signed_cmp,
              Int<16>& zext_out,
              Int<16>& sext_out,
              Int<8>& range_nested,
              Int<12>& cat_mix,
              Int<16>& repeat_mix) {
    unsigned_shift = a >> sh;
    signed_shift = a.sint() >> sh;
    signed_narrow_pos = Int<4>(Int<8>(0x7f).sint());
    signed_narrow_neg = Int<4>(Int<8>(0x80).sint());
    neg_const = Int<8>(-1);
    plain_cmp = Int<8>(0x80) > Int<8>(0);
    signed_cmp = Int<8>(0x80).sint() > Int<8>(0);
    zext_out = Int<16>(a);
    sext_out = Int<16>(a.sint());

    Int<8> tmp = a;
    tmp.at<3, 0>() = b.at<7, 4>();
    tmp.at<7>() = true;
    range_nested = tmp;

    cat_mix = Cat(a, Int<4>(b.at<3, 0>()));
    repeat_mix = Repeat<2>(a);
}
