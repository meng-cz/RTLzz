#include <fixint.hpp>

struct Bad {
    Int<8>& ref;
};

void consume(Bad value) {
}

#pragma input_port a
Int<8> a;
#pragma output_port out
Int<8> out;

void hls_main() {
    out = a;
}
