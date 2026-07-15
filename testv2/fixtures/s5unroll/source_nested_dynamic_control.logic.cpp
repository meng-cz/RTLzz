#include <cstdint>
#include <fixint.hpp>

void hls_main(bool skip_outer,
              bool stop_outer,
              bool skip_inner,
              bool stop_inner,
              Int<8>& out) {
    Int<8> acc = Int<8>(0);

    for (uint32_t i = 0; i < 2; ++i) {
        if (skip_outer) {
            continue;
        }
        if (stop_outer) {
            break;
        }

        for (uint32_t j = 0; j < 3; ++j) {
            if (skip_inner) {
                continue;
            }
            if (stop_inner) {
                break;
            }
            acc = acc + Int<8>(1);
        }
    }

    out = acc;
}
