#include <uint.hpp>

void hls_main(bool sel, Int<8>& out) {
    Int<8> tmp;
    if (sel) {
        tmp = Int<8>(1);
    }
    out = tmp;
}
