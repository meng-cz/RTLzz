#include <uint.hpp>

void hls_main(Int<8> a, Int<16> b, bool sel, Int<17>& out, bool& cmp) {
    Int<8> narrowed = b;
    Int<16> widened = a;
    Int<9> sum = a + narrowed;
    Int<16> prod = a * narrowed;
    out = sel ? Int<17>(sum) : Int<17>(prod + widened);
    cmp = a.sint() < narrowed.sint();
}
