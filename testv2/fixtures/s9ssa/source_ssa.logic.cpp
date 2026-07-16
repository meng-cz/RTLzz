#include <fixint.hpp>

void hls_main(Int<8> seed, int idx, Int<8>& out) {
    Int<8> arr[2];
    arr[0] = seed;
    arr[1] = Int<8>(3);

    out = arr[idx] + seed;
    arr[idx] = out;
}
