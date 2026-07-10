#include <array>
#include <fixint.hpp>

void hls_main(bool sel, Int<8> a, std::array<Int<8>, 2>& out) {
    Int<8> base = a;
    auto value = [=](Int<8> x) -> Int<8> {
        return x + Int<8>(1);
    };
    auto write = [&](Int<8> x) -> void {
        out[0] = x;
        if (sel) return;
        out[1] = x + Int<8>(3);
    };
    auto choose = [&](bool s, const Int<8>& x) -> Int<8> {
        return s ? (Int<8>(x.at<7,4>())) : (Int<8>(x.at<3,0>()));
    };
    write(value(base));
    out[1] = choose(sel, base);
}
