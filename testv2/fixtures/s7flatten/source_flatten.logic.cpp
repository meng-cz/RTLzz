#include <fixint.hpp>

struct Pair {
    Int<8> n;
    Int<8> m;
};

void hls_main(Int<8> seed, int idx, Int<8>& out) {
    Pair a;
    Pair b;
    Int<8> arr[3];

    a.n = seed;
    a.m = Int<8>(2);
    b = a;

    arr[0] = b.n;
    arr[1] = b.m;
    arr[2] = seed;

    out = arr[idx] + b.n;
    arr[idx] = out;
}
