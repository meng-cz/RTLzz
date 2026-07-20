#include <cstdint>
#include <fixint.hpp>

#pragma input_port a
Int<8> a;
#pragma input_port mode
uint8_t mode;
#pragma input_port skip
bool skip;
#pragma input_port stop
bool stop;
#pragma output_port out
Int<8> out;

void hls_main() {
    Int<8> acc = Int<8>(0);
    for (uint32_t i = 0; i < 4; ++i) {
        if (skip && i == 2) {
            continue;
        }
        acc = acc + a;
        if (stop && i == 2) {
            break;
        }
    }

    switch (mode & 3) {
    case 0:
        out = acc;
        break;
    case 1:
        out = acc + Int<8>(1);
        break;
    default:
        out = acc + Int<8>(2);
        break;
    }
}
