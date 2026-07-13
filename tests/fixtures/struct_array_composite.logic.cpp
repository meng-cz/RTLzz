#include <array>
#include <cstdint>
#include <fixint.hpp>

struct Cell {
    Int<8> c;
    bool flag;
};

struct ScalarArrayBox {
    std::array<Int<8>, 4> b;
};

struct CellArrayBox {
    std::array<Cell, 4> b;
};

struct Elem {
    Int<8> b;
    Cell nested;
};

void hls_main(Int<8> in0,
              Int<8> in1,
              Int<8> in2,
              Int<8> in3,
              Int<8> idx,
              bool sel,
              Int<8>& out_ab_i,
              Int<8>& out_ai_b,
              Int<8>& out_ab_i_c,
              bool& out_flag_mix) {
    ScalarArrayBox scalar_box;
    scalar_box.b[0] = in0;
    scalar_box.b[1] = in1;
    scalar_box.b[2] = in2;
    scalar_box.b[3] = in3;

    std::array<Elem, 4> elems;
    elems[0].b = in3 + Int<8>(1);
    elems[1].b = in2 + Int<8>(2);
    elems[2].b = in1 + Int<8>(3);
    elems[3].b = in0 + Int<8>(4);
    elems[0].nested.c = in0 ^ in1;
    elems[1].nested.c = in1 ^ in2;
    elems[2].nested.c = in2 ^ in3;
    elems[3].nested.c = in3 ^ in0;
    elems[0].nested.flag = sel;
    elems[1].nested.flag = !sel;
    elems[2].nested.flag = in0 < in1;
    elems[3].nested.flag = in2 < in3;

    CellArrayBox cell_box;
    cell_box.b[0].c = elems[0].nested.c + Int<8>(5);
    cell_box.b[1].c = elems[1].nested.c + Int<8>(6);
    cell_box.b[2].c = elems[2].nested.c + Int<8>(7);
    cell_box.b[3].c = elems[3].nested.c + Int<8>(8);
    cell_box.b[0].flag = elems[0].nested.flag;
    cell_box.b[1].flag = elems[1].nested.flag;
    cell_box.b[2].flag = elems[2].nested.flag;
    cell_box.b[3].flag = elems[3].nested.flag;

    out_ab_i = scalar_box.b[idx.at<1, 0>()];
    out_ai_b = elems[idx.at<1, 0>()].b;
    out_ab_i_c = cell_box.b[idx.at<1, 0>()].c;
    out_flag_mix = cell_box.b[idx.at<1, 0>()].flag ^ elems[idx.at<1, 0>()].nested.flag;
}
