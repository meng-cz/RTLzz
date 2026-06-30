#include <array>
#include <fixint.hpp>

void choose_pair(bool sel, Int<8>& x, Int<8>& y) {
    if (sel) {
        x = Int<8>(1);
        y = Int<8>(3);
        return;
    } else {
        x = Int<8>(2);
        y = Int<8>(4);
        return;
    }
    x = Int<8>(99);
}

void nested_return(bool outer, bool inner, Int<8>& x) {
    if (outer) {
        if (inner) {
            return;
        }
        x = x + Int<8>(5);
    }
    x = x + Int<8>(1);
}

void hls_main(bool sel,
              bool inner,
              Int<8> seed,
              Int<8>& out_x,
              Int<8>& out_y,
              Int<8>& tail,
              std::array<Int<8>, 2>& arr) {
    Int<8> x = seed;
    Int<8> y = Int<8>(0);
    choose_pair(sel, x, y);
    nested_return(sel, inner, x);
    arr[0] = Int<8>(0);
    arr[1] = Int<8>(0);

    auto write_array = [&](bool skip, Int<8>& value) -> void {
        if (skip) {
            return;
        }
        arr[0] = value;
        arr[1] = value + Int<8>(1);
    };

    write_array(inner, x);
    out_x = x;
    out_y = y;
    tail = Int<8>(9);
}
