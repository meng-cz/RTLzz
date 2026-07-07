#include <array>
#include <fixint.hpp>

using Vec4 = std::array<Int<8>, 4>;

void fill_array(Vec4& out, Int<8> seed, bool sel) {
    out[0] = seed + Int<8>(1);
    out[1] = seed + Int<8>(2);
    Int<8> first = out[0];
    Int<8> second = out[1];
    if (sel) {
        out[2] = first ^ second;
    } else {
        out[2] = seed + Int<8>(3);
    }
    Int<8> third = out[2];
    out[3] = third ^ Int<8>(4);
}

Vec4 make_array(Int<8> seed, bool sel) {
    Vec4 tmp{};
    tmp[0] = seed + Int<8>(5);
    tmp[1] = seed + Int<8>(6);
    Int<8> first = tmp[0];
    Int<8> second = tmp[1];
    if (sel) {
        tmp[2] = first ^ second;
    } else {
        tmp[2] = seed + Int<8>(7);
    }
    Int<8> third = tmp[2];
    tmp[3] = third ^ Int<8>(8);
    return tmp;
}

Vec4 mix_array(const Vec4& in, Int<8> delta) {
    Vec4 tmp{};
    Int<8> in0 = in[0];
    Int<8> in1 = in[1];
    Int<8> in2 = in[2];
    Int<8> in3 = in[3];
    tmp[0] = in0 + delta;
    tmp[1] = in1 ^ delta;
    tmp[2] = in2 + Int<8>(1);
    tmp[3] = in3 ^ Int<8>(2);
    return tmp;
}

void hls_main(Int<8> input,
              bool sel,
              Int<8>& out0,
              Int<8>& out1,
              Int<8>& out2,
              Int<8>& out3,
              Int<8>& checksum) {
    Vec4 base{};
    fill_array(base, input, sel);

    auto tweak = [&](Vec4& arr, Int<8> delta) -> void {
        Int<8> first = arr[0];
        Int<8> last = arr[3];
        arr[0] = first + delta;
        arr[3] = last ^ delta;
    };
    tweak(base, Int<8>(9));

    Vec4 made = make_array(base[0], sel);
    Vec4 mixed = mix_array(made, Int<8>(10));

    auto lambda_array = [&](const Vec4& in, Int<8> delta) -> Vec4 {
        Vec4 tmp{};
        Int<8> in0 = in[0];
        Int<8> in1 = in[1];
        Int<8> in2 = in[2];
        Int<8> in3 = in[3];
        tmp[0] = in0 ^ delta;
        tmp[1] = in1 + delta;
        tmp[2] = in2 ^ Int<8>(3);
        tmp[3] = in3 + Int<8>(4);
        return tmp;
    };
    Vec4 final = lambda_array(mixed, Int<8>(11));

    out0 = final[0];
    out1 = final[1];
    out2 = final[2];
    out3 = final[3];
    Int<8> base0 = base[0];
    Int<8> base1 = base[1];
    Int<8> final0 = final[0];
    Int<8> final1 = final[1];
    Int<8> final2 = final[2];
    Int<8> final3 = final[3];
    Int<8> base_mix = base0 ^ base1;
    Int<8> final_mix0 = final0 ^ final1;
    Int<8> final_mix1 = final2 ^ final3;
    checksum = base_mix ^ final_mix0 ^ final_mix1;
}
