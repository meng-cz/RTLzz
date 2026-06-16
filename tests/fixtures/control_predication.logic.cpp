#include <uint.hpp>

void hls_main(Int<8> a, uint8_t mode, bool s0, bool s1, Int<8>& out) {
    Int<8> acc = Int<8>(0);
    for (uint32_t i = 0; i < 4; ++i) {
        if (s0 && i == 2) continue;
        acc = acc + a;
        if (s1 && i == 2) break;
    }
    switch (mode & 3) {
    case 0:
        out = acc;
        break;
    case 1:
        out = acc + Int<8>(1);
        break;
    default:
        out = acc + Int<8>(2);
        break;
    }
}
