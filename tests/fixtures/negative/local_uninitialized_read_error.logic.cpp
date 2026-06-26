#include <cstdint>

void hls_main(bool sel, uint8_t& out) {
    uint8_t tmp;
    if (sel) {
        tmp = 1;
    }
    out = tmp;
}
