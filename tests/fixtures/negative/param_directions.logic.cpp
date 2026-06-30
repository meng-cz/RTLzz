#include <fixint.hpp>

void hls_main(const Int<8>& input_ref, Int<8>& inout_ref, Int<8>& output_ref, bool& valid) {
    Int<8> old_value = inout_ref;
    inout_ref = old_value + input_ref;
    output_ref = input_ref;
    valid = true;
}
