#include <fixint.hpp>

#pragma input_port seed
Int<8> seed;
#pragma output_port out
Int<8> out;
#pragma output_port flag
bool flag;

void hls_main() {
    constexpr int WIDTH = 8;
    constexpr int HI_LO = 4;
    auto high_half = [&]() -> Int<4> {
        return Int<4>(seed.at<WIDTH - 1, HI_LO>());
    };
    Int<8> n = seed;
    n.at<3, 0>() = Int<4>(1);
    flag = n.at<3>();
    out = n ^ Int<8>(high_half());
}
