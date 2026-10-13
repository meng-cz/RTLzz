#include <fixint.hpp>

#pragma input_port seed
Int<8> seed;
#pragma input_port idx
int idx;
#pragma output_port out
Int<8> out;

void hls_main() {
    Int<8> arr[2];
    arr[0] = seed;
    arr[1] = Int<8>(3);

    out = arr[idx] + seed;
    arr[idx] = out;
}
