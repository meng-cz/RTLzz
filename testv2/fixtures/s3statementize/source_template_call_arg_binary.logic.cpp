#include <cstdint>
#include <fixint.hpp>

#pragma input_port in
Int<32> in;
#pragma output_port out
Int<32> out;

uint32_t read_value() {
    return in.template to<uint32_t>();
}

template <uint32_t P = 0>
void write_value(uint32_t value) {
    out = Int<32>(value);
}

void hls_main() {
    auto tick = [&]() {
        write_value<0>(read_value() + 1);
    };
    tick();
}
