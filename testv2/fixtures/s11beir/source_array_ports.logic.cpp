#include <array>
#include <fixint.hpp>

void hls_main(const std::array<Int<8>, 2>& in,
              Int<8>& selected) {
    selected = in[1] + Int<8>(1);
}
