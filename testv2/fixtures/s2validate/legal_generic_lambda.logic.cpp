#include <cstdint>
#include <fixint.hpp>

#pragma input_port in
Int<32> in;
#pragma output_port out0
Int<32> out0;
#pragma output_port out1
Int<32> out1;

void hls_main() {
    constexpr int64_t LANES = 2;
    out0 = 0;
    out1 = 0;

    auto route = [&]<uint32_t IDX = 0>(uint32_t data) -> void {
        static_assert(IDX < LANES);
        if constexpr (IDX == 0) {
            out0 = Int<32>(data);
        } else if constexpr (IDX == 1) {
            out1 = Int<32>(data);
        }
    };

    uint32_t data =
        in.template to<uint32_t>();
    route.template operator()<0>(data);
    route.template operator()<1>(data + 1);
}
