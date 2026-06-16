#include <uint.hpp>

Int<8> choose(bool sel, Int<8> a, Int<8> b) {
    Int<8> base = sel ? a : b;
    return base + Int<8>(1);
}

void bump(Int<8>& value) {
    value = value + Int<8>(1);
}

void hls_main(bool sel, Int<8> a, Int<8> b, Int<8>& out, Int<8>& tail) {
    Int<8> tmp = choose(sel, a, b);
    bump(tmp);
    out = tmp;
    tail = Int<8>(9);
}
