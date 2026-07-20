#include <fixint.hpp>

#pragma input_port seed
Int<8> seed;
#pragma output_port out
Int<8> out;
#pragma output_port flag
bool flag;

void hls_main() {
    Int<8> n = seed;
    n.at<3, 0>() = Int<4>(1);
    flag = n.at<3>();
    out = n;
}
