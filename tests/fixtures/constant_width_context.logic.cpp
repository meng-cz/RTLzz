#include <cstdint>
#include <uint.hpp>

void hls_main(uint8_t a,
              uint16_t b,
              bool sel,
              uint16_t& add8_out,
              uint32_t& add16_out,
              bool& cmp_out) {
    add8_out = a + 1;
    add16_out = b + (sel ? 0 : b);
    cmp_out = a < 10;
}
