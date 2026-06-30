#include <array>
#include <fixint.hpp>

void hls_main(std::array<Int<8>, 4> in, Int<8> idx, Int<8>& out, bool& bit) {
    out = in[idx.at<1, 0>()];
    bit = out.pick(idx.at<2, 0>());
}
