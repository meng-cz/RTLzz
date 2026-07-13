#include <fixint.hpp>

void hls_main(Int<8> a,
              Int<8>& out,
              Int<8>& folded,
              bool& known_zero,
              Int<32>& and_mask_noop,
              Int<32>& or_mask_noop,
              Int<32>& xor_zero_noop,
              Int<32>& double_not_noop) {
    Int<32> wide = Int<32>(a);
    out = Int<8>(wide);

    Int<8> c = Int<8>(15) | Int<8>(240);
    folded = c;
    known_zero = ReduceOr(Int<8>(0));

    and_mask_noop = wide & Int<32>(0xfff);

    Int<32> known_ones = wide | Int<32>(0xff00);
    or_mask_noop = known_ones | Int<32>(0xf00);

    Int<32> known_zero_masked = wide & Int<32>(0xff00);
    xor_zero_noop = wide ^ known_zero_masked;

    double_not_noop = ~~wide;
}
