#include <cstdint>
#include <fixint.hpp>

#pragma input_port skip_outer
bool skip_outer;
#pragma input_port stop_outer
bool stop_outer;
#pragma input_port skip_inner
bool skip_inner;
#pragma input_port stop_inner
bool stop_inner;
#pragma output_port out
Int<8> out;

void hls_main() {
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
