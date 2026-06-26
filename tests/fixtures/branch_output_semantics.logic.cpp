#include <uint.hpp>

void hls_main(Int<8> a, Int<8> b, Int<8>& out) {
    if (a < b) {
        out = a + b;
    } else {
        out = b;
        out(3, 0) = a(3, 0);
    }
}
