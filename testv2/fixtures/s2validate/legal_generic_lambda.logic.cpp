#include <cstdint>
#include <fixint.hpp>

constexpr int64_t LANES = 2;

void hls_main(Int<32> in, Int<32>& out0, Int<32>& out1) {
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
