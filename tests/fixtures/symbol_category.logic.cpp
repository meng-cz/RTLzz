#include <uint.hpp>

void hls_main(Int<8> a, Int<8>& out) {
    const Int<8> local_const = Int<8>(3);
    Int<8> local = a + local_const;
    out = local;
}
