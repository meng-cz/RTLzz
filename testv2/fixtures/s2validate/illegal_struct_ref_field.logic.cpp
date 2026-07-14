#include <fixint.hpp>

struct Bad {
    Int<8>& ref;
};

void consume(Bad value) {
}

void hls_main(Int<8> a, Int<8>& out) {
    out = a;
}
