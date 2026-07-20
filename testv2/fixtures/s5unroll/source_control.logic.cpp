#include <cstdint>
#include <fixint.hpp>

#pragma input_port step
Int<8> step;
#pragma input_port stop
bool stop;
#pragma output_port out
Int<8> out;

void hls_main() {
    Int<8> acc = Int<8>(0);

    for (uint32_t i = 0; i < 3; ++i) {
        acc = acc + step;
    }

    for (uint32_t i = 0; i < 2; ++i) {
        for (uint32_t j = 0; j < 3; ++j) {
            acc = acc + step;
        }
    }

    for (uint32_t i = 0; i < 4; ++i) {
        if (stop) {
            break;
        }
        acc = acc + step;
    }

    for (uint32_t i = 0; i < 4; ++i) {
        if (i == 1) {
            continue;
        }
        acc = acc + step;
    }

    for (uint32_t i = 0; i < 5; ++i) {
        if (i == 1) {
            continue;
        }
        if (i == 3) {
            break;
        }
        acc = acc + step;
    }

    out = acc;
}
