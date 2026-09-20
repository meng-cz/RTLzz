#include <fixint.hpp>

#pragma input_port base
Int<32> base;
#pragma input_port a
Int<8> a;
#pragma input_port b
Int<8> b;
#pragma input_port c
Int<8> c;
#pragma output_port adjacent
Int<32> adjacent;
#pragma output_port overlap
Int<32> overlap;
#pragma output_port sparse
Int<32> sparse;

void hls_main() {
    Int<32> value = base;
    value.at<7, 0>() = a;
    value.at<15, 8>() = b;
    value.at<23, 16>() = c;
    adjacent = value;

    value = base;
    value.pick<12>(0) = Int<12>(a);
    value.pick<12>(4) = Int<12>(b);
    overlap = value;

    value = base;
    value.at<7, 0>() = a;
    value.at<31, 24>() = c;
    sparse = value;
}
