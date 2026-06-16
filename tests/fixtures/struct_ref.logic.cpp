#include <uint.hpp>

struct RefPair {
    Int<8>& x;
    bool& v;
    RefPair(bool& v_ref, Int<8>& x_ref) : x(x_ref), v(v_ref) {}
};

void hls_main(Int<8> input, Int<8>& out_x, bool& out_v, Int<8>& copied) {
    RefPair pair(out_v, out_x);
    pair.x = input;
    pair.v = true;
    copied = input + Int<8>(1);
}
