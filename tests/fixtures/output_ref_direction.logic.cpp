#include <uint.hpp>

void helper(const Int<8>& input, bool& valid, Int<8>& payload) {
    valid = true;
    payload = input;
}

void hls_main(const Int<8>& input, bool& output__vld__, Int<8>& output_s__) {
    helper(input, output__vld__, output_s__);
}
