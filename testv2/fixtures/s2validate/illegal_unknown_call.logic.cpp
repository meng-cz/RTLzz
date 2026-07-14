#include <fixint.hpp>

Int<8> missing_helper(Int<8> x);

void hls_main(Int<8> a, Int<8>& out) {
    out = missing_helper(a);
}
