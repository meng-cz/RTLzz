#include <fixint.hpp>

Int<8> choose(bool sel, Int<8> a, Int<8> b) {
    if (sel) {
        return a + Int<8>(1);
    }
    return b + Int<8>(2);
}

Int<8> adjust(bool sel, Int<8> value) {
    if (sel) {
        return value + Int<8>(4);
    }
    return value + Int<8>(5);
}

void touch(Int<8>& value) {
    value = value + Int<8>(3);
}

Int<8> lam(Int<8> x) {
    return x + Int<8>(6);
}

void hls_main(bool sel, Int<8> a, Int<8> b, Int<8>& out, Int<8>& tail) {
    Int<8> t = choose(sel, a, b);
    touch(t);
    out = adjust(sel, t);

    if (sel) {
        tail = lam(t);
    } else {
        tail = b;
    }
}
