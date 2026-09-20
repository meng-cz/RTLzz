#include <fixint.hpp>
#pragma input_port selector
Int<3> selector;
#pragma input_port a
Int<8> a;
#pragma input_port b
Int<8> b;
#pragma input_port c
Int<8> c;
#pragma input_port d
Int<8> d;
#pragma input_port wide
Int<128> wide;
#pragma output_port wide_mux
Int<128> wide_mux;
#pragma output_port exclusive
Int<8> exclusive;
#pragma output_port priority_result
Int<8> priority_result;
#pragma output_port tree_xor
Int<8> tree_xor;
#pragma output_port tree_add
Int<8> tree_add;
#pragma output_port nested
Int<8> nested;
void hls_main() {
    exclusive = selector == Int<3>(0) ? a :
                selector == Int<3>(1) ? b :
                selector == Int<3>(2) ? c :
                selector == Int<3>(3) ? d : Int<8>(93);
    wide_mux = selector == Int<3>(0) ? wide :
               selector == Int<3>(1) ? ~wide :
               selector == Int<3>(2) ? Int<128>(a) : Int<128>(93);
    priority_result = (a != Int<8>(0)) ? b : (b != Int<8>(0)) ? c : d;
    tree_xor = (((((a ^ b) ^ c) ^ d) ^ Int<8>(17)) ^ Int<8>(93));
    Int<8> sum = Int<8>(a + b);
    sum = Int<8>(sum + c);
    sum = Int<8>(sum + d);
    sum = Int<8>(sum + Int<8>(17));
    tree_add = sum;
    Int<8> inner = ((selector == Int<3>(0)) && (a != Int<8>(0))) ? b : c;
    nested = selector == Int<3>(0) ? (a != Int<8>(0) ? inner : d) : a;
}
