#include <fixint.hpp>

struct Packet {
    Int<8> data;
};

#pragma input_port in
Packet in;
#pragma output_port out
Int<8> out;

void hls_main() {
    out = in.data;
}
