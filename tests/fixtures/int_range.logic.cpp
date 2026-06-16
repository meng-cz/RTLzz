#include <uint.hpp>

void hls_main(Int<32> word, Int<8> value, Int<32>& out, bool& selected) {
    Int<32> next = word;
    Int<8> low = word(7, 0);
    selected = word(3);
    next(15, 8) = value;
    next(7, 0) = low;
    next(0) = selected;
    out = next;
}
