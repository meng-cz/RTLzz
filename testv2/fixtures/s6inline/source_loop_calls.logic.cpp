#include <fixint.hpp>

Int<8> inc(Int<8> x) {
    return x + Int<8>(1);
}

void bump(Int<8>& x) {
    x = inc(x);
}

void hls_main(Int<8> seed, Int<8>& out) {
    Int<8> acc = seed;
    for (int i = 0; i < 3; i = i + 1) {
        acc = inc(acc);
        bump(acc);
    }
    out = acc;
}
