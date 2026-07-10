#include <fixint.hpp>

struct Packet {
    Int<8> x;
    Int<8> y;
    bool flag;
};

void fill_packet(Packet& p, Int<8> seed, bool sel) {
    p.x = seed + Int<8>(1);
    if (sel) {
        p.y = seed + Int<8>(2);
    } else {
        p.y = seed + Int<8>(3);
    }
    p.flag = sel;
}

Packet make_packet(Int<8> seed, bool sel) {
    Packet p;
    p.x = seed + Int<8>(5);
    if (sel) {
        p.y = seed + Int<8>(6);
    } else {
        p.y = seed + Int<8>(7);
    }
    p.flag = !sel;
    return p;
}

Packet mix_packet(const Packet& p, Int<8> delta) {
    Packet out;
    out.x = p.x + delta;
    out.y = p.y + Int<8>(1);
    out.flag = !p.flag;
    return out;
}

void hls_main(Int<8> input,
              bool sel,
              Int<8>& out_x,
              Int<8>& out_y,
              bool& out_flag,
              Int<8>& out_sum) {
    Packet pkt = Packet{input, Int<8>(0), false};

    // pkt.x = input;
    // pkt.y = Int<8>(0);
    // pkt.flag = false;

    fill_packet(pkt, pkt.x, sel);

    auto tweak = [&](Packet& q, Int<8> delta) -> void {
        q.x = q.x + delta;
        q.flag = !q.flag;
    };
    tweak(pkt, Int<8>(4));

    Packet helper_ret = make_packet(pkt.x, pkt.flag);
    Packet helper_param_ret = mix_packet(helper_ret, Int<8>(2));

    auto lambda_packet = [&](const Packet& p, Int<8> delta) -> Packet {
        Packet out;
        out.x = p.x + delta;
        out.y = p.y + Int<8>(2);
        out.flag = !p.flag;
        return out;
    };
    Packet lambda_ret = lambda_packet(helper_param_ret, Int<8>(3));

    out_x = lambda_ret.x;
    out_y = lambda_ret.y;
    out_flag = lambda_ret.flag;
    out_sum = pkt.x ^ pkt.y ^ lambda_ret.x ^ lambda_ret.y;
}
