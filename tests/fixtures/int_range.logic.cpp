#include <fixint.hpp>

void hls_main(Int<32> word, Int<8> value, Int<4> dyn_idx, Int<32>& out, bool& selected) {
    Int<32> next = word;
    Int<8> low = word.at<7, 0>();
    selected = word.at<3>();
    next.at<15, 8>() = value;
    next.at<7, 0>() = low;
    next.at<0>() = selected;
    next.pick<8>(dyn_idx) = value;
    next.pick(dyn_idx) = selected;
    out = next;
}
