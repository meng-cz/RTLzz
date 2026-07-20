#include <array>
#include <fixint.hpp>

#pragma input_port in
std::array<Int<8>, 2> in;
#pragma output_port selected
Int<8> selected;

void hls_main() {
    selected = in[1];
}
