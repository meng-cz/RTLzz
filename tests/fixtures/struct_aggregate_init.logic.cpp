#include <fixint.hpp>

struct PacketAgg {
    Int<8> x;
    Int<8> y;
    bool flag;
};

void hls_main(Int<8> input,
              bool sel,
              Int<8>& out_x,
              Int<8>& out_y,
              bool& out_flag,
              Int<8>& out_mix) {
    PacketAgg ordered = PacketAgg{input, Int<8>(1), sel};
    PacketAgg named = PacketAgg{.flag = false, .x = input};

    out_x = ordered.x;
    out_y = named.y;
    out_flag = ordered.flag ^ named.flag;
    out_mix = named.x + ordered.y;
}
