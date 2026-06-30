#include <array>
#include <cstdint>
#include <fixint.hpp>

void hls_main(std::array<Int<8>, 4> in, Int<8> idx, std::array<Int<8>, 4>& out, Int<8>& selected) {
    std::array<Int<8>, 4> tmp{0};
    for (uint32_t i = 0; i < 4; ++i) {
        tmp[i] = in[i];
        out[i] = tmp[i];
    }
    selected = tmp[idx.at<1, 0>()];
}
