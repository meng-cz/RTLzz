#include <fixint.hpp>

void hls_main(Int<8> seed, Int<3> idx, Int<4>& out, bool& bit) {
    out = seed.pick<4>(idx);
    bit = seed.pick(idx);
}
