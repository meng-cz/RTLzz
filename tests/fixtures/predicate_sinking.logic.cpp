#include <fixint.hpp>

void hls_main(Int<16> a,
              Int<16> b,
              bool flag,
              bool sel,
              bool extra,
              Int<16>& out0,
              Int<16>& out1,
              Int<16>& out2) {
    Int<16> left = Int<16>(0);
    Int<16> right = Int<16>(0);
    Int<16> mixed = Int<16>(0);
    Int<16> shadow = Int<16>(0);

    if (flag) {
        left = a + Int<16>(1);
        right = b ^ Int<16>(0x55);
        if (sel) {
            mixed = left + right;
        } else {
            mixed = left - right;
        }
        if (extra) {
            shadow = mixed | Int<16>(3);
        } else {
            shadow = mixed & Int<16>(0xff);
        }
    }

    if (flag) {
        out0 = left + Int<16>(2);
        out1 = right ^ shadow;
        out2 = mixed + shadow;
    } else {
        out0 = Int<16>(0);
        out1 = Int<16>(0);
        out2 = Int<16>(0);
    }
}
