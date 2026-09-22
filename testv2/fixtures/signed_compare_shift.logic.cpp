#include <cstdint>
#include <fixint.hpp>

#pragma input_port lhs
int32_t lhs;
#pragma input_port rhs
int32_t rhs;

#pragma output_port ge_var
bool ge_var;
#pragma output_port gt_var
bool gt_var;
#pragma output_port lt_var
bool lt_var;
#pragma output_port ge_zero
bool ge_zero;
#pragma output_port lt_neg_1022
bool lt_neg_1022;
#pragma output_port shifted
Int<32> shifted;
#pragma output_port result_scale
Int<32> result_scale;

int32_t signed_identity(int32_t value) {
    return value;
}

void hls_main() {
    int32_t a = signed_identity(lhs);
    int32_t b = signed_identity(rhs);
    ge_var = a >= b;
    gt_var = a > b;
    lt_var = a < b;
    ge_zero = a >= 0;
    lt_neg_1022 = a < -1022;
    shifted = Int<32>(a >> 1);
    result_scale = Int<32>((a >> 1) - 32);
}
