#include <array>
#include <fixint.hpp>

#pragma input_port idx
int idx;
#pragma input_port seed
Int<8> seed;
#pragma output_port out
Int<8> out;

void hls_main() {
    std::array<Int<8>, 2> arr;
    arr[idx] = seed;
    out = seed;
}
