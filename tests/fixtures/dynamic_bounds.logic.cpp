#include <array>
#include <uint.hpp>

void hls_main(std::array<Int<8>, 4> in, Int<8> idx, Int<8>& out, bool& bit) {
    out = in[idx(1, 0)];
    bit = out.bit_at(idx(2, 0));
}
