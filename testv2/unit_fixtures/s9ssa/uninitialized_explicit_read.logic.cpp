#include <fixint.hpp>

#pragma output_port out
Int<8> out;

void hls_main() {
    Int<8> value;
    out = value;
}
