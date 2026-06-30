#include <fixint.hpp>

void hls_main(Int<8> a, Int<8> b, Int<32>& out, bool& any, bool& all, bool& parity) {
    Int<16> joined = Cat(a, b);
    out = Repeat<2>(joined);
    any = ReduceOr(joined);
    all = ReduceAnd(joined);
    parity = ReduceXor(joined);
}
