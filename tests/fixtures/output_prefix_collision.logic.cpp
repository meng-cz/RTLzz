#include <array>
#include <cstdint>

using std::array;

void hls_main(const array<uint16_t, 8>& src, array<uint16_t, 8>& C_top) {
    array<uint16_t, 8> C_top_tmp{0};
    for (uint32_t i = 0; i < 8; ++i) {
        C_top_tmp[i] = src[i];
    }
    C_top = C_top_tmp;
}
