#include <fixint.hpp>

struct Packet {
    Int<8> data;
};

void hls_main(Packet in, Int<8>& out) {
    out = in.data;
}
