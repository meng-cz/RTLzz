#include <fixint.hpp>

Int<8> add1(Int<8> x) {
    return x + Int<8>(1);
}

#pragma input_port a
Int<8> a;
#pragma output_port out
Int<8> out;

void hls_main() {
    out = add1(a);
}
