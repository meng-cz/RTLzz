#include <fixint.hpp>

Int<8> missing_helper(Int<8> x);

#pragma input_port a
Int<8> a;
#pragma output_port out
Int<8> out;

void hls_main() {
    out = missing_helper(a);
}
