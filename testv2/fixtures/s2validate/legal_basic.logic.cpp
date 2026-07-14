#include <fixint.hpp>

Int<8> add1(Int<8> x) {
    return x + Int<8>(1);
}

void hls_main(Int<8> a, Int<8>& out) {
    out = add1(a);
}
