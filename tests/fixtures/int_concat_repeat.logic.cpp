#include <uint.hpp>

void hls_main(Int<8> a, Int<8> b, Int<32>& out, bool& any, bool& all, bool& parity) {
    Int<16> joined = a.cat(b);
    out = joined.repeat<2>();
    any = joined.reduce_or();
    all = joined.reduce_and();
    parity = joined.reduce_xor();
}
