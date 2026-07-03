#include <cstdint>
#include <fixint.hpp>

void hls_main(Int<16> word,
              Int<8> narrow,
              Int<8>& trunc_u,
              Int<32>& extend_u,
              Int<32>& extend_s,
              Int<8>& same_s) {
    uint8_t low = word.template to<uint8_t>();
    uint32_t widened = narrow.template to<uint32_t>();
    int32_t signed_widened = narrow.template to<int32_t>();
    int8_t signed_same = narrow.template to<int8_t>();

    trunc_u = low;
    extend_u = widened;
    extend_s = signed_widened;
    same_s = signed_same;
}
