#include <cstdint>
#include <uint.hpp>

void hls_main(Int<8> idx, Int<8>& out_static, Int<8>& out_dynamic) {
    static constexpr uint8_t LUT4[4] = {3, 1, 4, 2};
    constexpr int fixed_index = 2;
    out_static = Int<8>(LUT4[fixed_index]);
    out_dynamic = Int<8>(LUT4[idx(1, 0)]);
}
