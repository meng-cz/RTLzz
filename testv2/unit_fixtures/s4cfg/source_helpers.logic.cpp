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

#pragma input_port sel
bool sel;
#pragma input_port a
Int<8> a;
#pragma input_port b
Int<8> b;
#pragma output_port out
Int<8> out;
#pragma output_port tail
Int<8> tail;

void hls_main() {
    Int<8> t = choose(sel, a, b);
    touch(t);
    out = adjust(sel, t);

    if (sel) {
        tail = t;
    } else {
        tail = b;
    }
}
