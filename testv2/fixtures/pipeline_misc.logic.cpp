#include <array>
#include <fixint.hpp>

Int<8> a;
#pragma input_port a
Int<8> b;
#pragma input_port b
bool choose;
#pragma input_port choose
Int<2> index;
#pragma input_port index
std::array<std::array<Int<8>, 2>, 2> values;
#pragma input_port values

Int<8> direct;
#pragma output_port direct
Int<8> chain;
#pragma output_port chain
Int<16> product;
#pragma output_port product
Int<8> lookup;
#pragma output_port lookup
std::array<std::array<Int<8>, 2>, 2> copies;
#pragma output_port copies
Int<8> constant;
#pragma output_port constant

void hls_main() {
    Int<8> shared = a + b;
    Int<8> next = shared ^ Int<8>(0x5a);
    Int<8> last = next + b;
    direct = a;
    chain = choose ? last : shared;
    product = choose ? Int<16>(a.sint() * b.sint()) : Int<16>(shared) + Int<16>(last);
    std::array<Int<8>, 4> dynamic_table = {a, b, shared, last};
    lookup = dynamic_table[index.to<unsigned>()];
    copies = values;
    constant = Int<8>(42);
}
