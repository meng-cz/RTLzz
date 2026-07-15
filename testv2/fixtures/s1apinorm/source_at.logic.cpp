#include <fixint.hpp>

void hls_main(Int<8> seed, Int<8>& out, bool& flag) {
    Int<8> n = seed;
    n.at<3, 0>() = Int<4>(1);
    flag = n.at<3>();
    out = n;
}
