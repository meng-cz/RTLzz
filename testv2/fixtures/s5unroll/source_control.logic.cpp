#include <cstdint>
#include <fixint.hpp>

void hls_main(Int<8> step, bool stop, Int<8>& out) {
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
