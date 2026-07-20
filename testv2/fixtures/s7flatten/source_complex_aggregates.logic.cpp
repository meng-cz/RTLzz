#include <fixint.hpp>

struct Pair {
    Int<8> n;
    Int<8> m;
};

struct Packet {
    Pair lanes[2];
    Int<8> tail;
};

struct Box {
    Packet pkts[2];
};

Packet make_packet(Int<8> seed) {
    Packet pkt;
    pkt.lanes[0].n = seed;
    pkt.lanes[0].m = seed + Int<8>(1);
    pkt.lanes[1].n = seed + Int<8>(2);
    pkt.lanes[1].m = seed + Int<8>(3);
    pkt.tail = seed + Int<8>(4);
    return pkt;
}

void touch_pair(Pair& value, Int<8> replacement) {
    value.m = replacement;
}

void touch_packet(Packet& pkt, int idx, Int<8> replacement) {
    Pair local;
    local = pkt.lanes[idx];
    touch_pair(local, replacement);
    pkt.lanes[idx] = local;
}

Pair select(Packet pkt_arg, int idx_arg) {
    Pair local;
    local = pkt_arg.lanes[idx_arg];
    return local;
}

#pragma input_port seed
Int<8> seed;
#pragma input_port idx
int idx;
#pragma output_port out
Int<8> out;

void hls_main() {
    Packet pkt;
    pkt = make_packet(seed);
    Pair chosen;
    chosen = select(pkt, idx);

    touch_packet(pkt, idx, chosen.m);

    Box box;
    box.pkts[0] = pkt;
    box.pkts[1] = make_packet(chosen.n);

    out = box.pkts[idx].lanes[1].m + pkt.lanes[idx].n + chosen.m;
}
