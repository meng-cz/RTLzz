#include <fixint.hpp>

Int<8> choose(bool sel, Int<8> a, Int<8> b) {
    Int<8> base = sel ? a : b;
    return base + Int<8>(1);
}

void bump(Int<8>& value) {
    value = value + Int<8>(1);
}

Int<8> choose_other(const bool& sel, const Int<8>& a, const Int<8>& b) {
    Int<8> base = sel ? b : a;
    return base + Int<8>(2);
}

void hls_main(bool sel, Int<8> a, Int<8> b, Int<8>& out, Int<8>& tail) {
    Int<8> tmp = choose(sel, a, b);
    bump(tmp);
    Int<8> tmp2 = choose_other(sel, a, b);
    out = tmp + tmp2;
    tail = Int<8>(9);
}
