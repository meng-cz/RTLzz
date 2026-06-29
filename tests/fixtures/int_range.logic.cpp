#include <uint.hpp>

void hls_main(Int<32> word, Int<8> value, Int<4> dyn_idx, Int<32>& out, bool& selected) {
    Int<32> next = word;
    Int<8> low = word(7, 0);
    selected = word(3);
    next(15, 8) = value;
    next(7, 0) = low;
    next(0) = selected;
    next.range_at<8>(dyn_idx) = value;
    next.bit_at(dyn_idx) = selected;
    out = next;
}
