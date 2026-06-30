#include <cstdint>
#include <fixint.hpp>

void hls_main(Int<8> idx, Int<8>& out_static, Int<8>& out_dynamic) {
    static constexpr uint8_t LUT4[4] = {3, 1, 4, 2};
    constexpr int fixed_index = 2;
    out_static = Int<8>(LUT4[fixed_index]);
    uint8_t lut_idx = idx.at<1, 0>();
    out_dynamic = Int<8>(LUT4[lut_idx]);
}
