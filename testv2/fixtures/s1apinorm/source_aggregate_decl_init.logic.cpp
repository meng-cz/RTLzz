#include <fixint.hpp>

struct Pair {
    Int<8> n;
    Int<8> m;
};

struct Box {
    Pair member;
};

#pragma input_port idx
int idx;
#pragma output_port out
Int<8> out;

void hls_main() {
    Box box;
    Pair arr[2];
    Pair from_member = box.member;
    Pair from_index = arr[idx];
    out = from_member.n + from_index.m;
}
