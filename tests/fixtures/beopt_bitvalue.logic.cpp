#include <fixint.hpp>

void hls_main(Int<8> a, Int<8>& out, Int<8>& folded, bool& known_zero) {
    Int<32> wide = Int<32>(a);
    out = Int<8>(wide);

    Int<8> c = Int<8>(15) | Int<8>(240);
    folded = c;
    known_zero = ReduceOr(Int<8>(0));
}
