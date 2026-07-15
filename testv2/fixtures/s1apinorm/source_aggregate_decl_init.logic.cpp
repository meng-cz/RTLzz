#include <fixint.hpp>

struct Pair {
    Int<8> n;
    Int<8> m;
};

struct Box {
    Pair member;
};

void hls_main(int idx, Int<8>& out) {
    Box box;
    Pair arr[2];
    Pair from_member = box.member;
    Pair from_index = arr[idx];
    out = from_member.n + from_index.m;
}
