#include <fixint.hpp>

#pragma input_port seed
Int<8> seed;
#pragma input_port idx
Int<3> idx;
#pragma output_port out
Int<4> out;
#pragma output_port bit
bool bit;

void hls_main() {
    out = seed.pick<4>(idx);
    bit = seed.pick(idx);
}
