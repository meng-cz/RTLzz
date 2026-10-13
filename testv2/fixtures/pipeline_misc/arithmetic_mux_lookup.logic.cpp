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
Int<32> word;
#pragma input_port word
Int<3> shift;
#pragma input_port shift
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

Int<32> deep_chain;
#pragma output_port deep_chain
Int<32> late_merge;
#pragma output_port late_merge
Int<32> arithmetic_shift;
#pragma output_port arithmetic_shift
Int<32> quotient;
#pragma output_port quotient
Int<4> remainder;
#pragma output_port remainder
Int<16> bit_patch;
#pragma output_port bit_patch
Int<4> dynamic_slice;
#pragma output_port dynamic_slice
Int<16> repeated_byte;
#pragma output_port repeated_byte
std::array<bool, 3> reductions;
#pragma output_port reductions
bool signed_compare;
#pragma output_port signed_compare
bool logic_compare;
#pragma output_port logic_compare

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

    // Four dependent mixing rounds retain a long path after BEOPT. Different
    // operators and data-dependent muxes prevent associative tree balancing
    // from turning this into a shallow chain of equivalent additions.
    Int<32> state = word ^ Cat(a, b, a, b);
    for (int round = 0; round < 4; ++round) {
        state = state + Cat(b, a, b, a) + Int<32>(round + 1);
        state = state ^ (state >> shift.to<unsigned>());
        state = state * Int<32>(0x045d9f3bu);
        state = (state << (5 + round)) | (state >> (27 - round));
        bool branch = Int<1>(state.at<7>()) != Int<1>(0);
        if (branch != choose) {
            state = state - Cat(a, b, a, b);
        } else {
            state = state + Cat(b, a, b, a);
        }
    }
    deep_chain = state;

    // Join the deep result with fresh inputs and shorter paths from the same
    // transaction. Conditions, array reads and data all need stage alignment.
    Int<32> side = Cat(values[0][0], values[1][1], shared, lookup);
    late_merge = choose ? (state ^ side) : (state + word);
    arithmetic_shift = state.sint() >> shift.to<unsigned>();
    signed_compare = state.sint() < word.sint();
    logic_compare = ((state == word) || (shared < a)) && !choose;

    // Power-of-two division/modulo cover their slice/shift lowering. Builtin
    // unsigned operations keep the C++ oracle semantics well defined.
    quotient = Int<32>(state.to<unsigned>() / 8u);
    remainder = Int<4>(state.to<unsigned>() % 8u);

    // Both static and bounded dynamic bit writes occur on initialized storage.
    // Dynamic reads cover offsets 0/8/16/24; writes cover 0/4/8/12, always in range.
    Int<16> patched = Int<16>(state.at<15, 0>());
    patched.at<7, 0>() = a ^ b;
    patched.pick<4>(index.to<unsigned>() * 4) = Int<4>(state.at<23, 20>());
    bit_patch = patched;
    dynamic_slice = state.pick<4>(index.to<unsigned>() * 8);
    repeated_byte = Repeat<2>(Int<8>(state.at<7, 0>()));
    // Small any/all reductions exercise both Boolean outcomes under random
    // inputs; the parity reduction still covers the complete deep result.
    reductions[0] = ReduceOr(Int<4>(state.at<3, 0>()));
    reductions[1] = ReduceAnd(Int<4>(state.at<7, 4>()));
    reductions[2] = ReduceXor(state);
}
