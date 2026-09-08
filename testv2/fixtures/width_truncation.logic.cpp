#include <cstdint>
#include <fixint.hpp>

struct NarrowFields {
    Int<5> five;
    Int<17> seventeen;
};

#pragma input_port wide
Int<32> wide;
#pragma input_port shift
Int<6> shift;

#pragma output_port mask4
Int<4> mask4;
#pragma output_port trunc5
Int<5> trunc5;
#pragma output_port trunc17
Int<17> trunc17;
#pragma output_port left5
Int<5> left5;
#pragma output_port logical17
Int<17> logical17;
#pragma output_port arithmetic17
Int<17> arithmetic17;
#pragma output_port constant_left5
Int<5> constant_left5;
#pragma output_port constant_arithmetic17
Int<17> constant_arithmetic17;
#pragma output_port signed_trunc5
Int<5> signed_trunc5;
#pragma output_port field5
Int<5> field5;
#pragma output_port field17
Int<17> field17;

void hls_main() {
    uint32_t address = wide.template to<uint32_t>();
    mask4 = Int<4>(1U << (address & 3U));

    trunc5 = Int<5>(wide);
    trunc17 = Int<17>(wide);
    left5 = Int<5>(wide << shift);
    logical17 = Int<17>(wide >> shift);
    arithmetic17 = Int<17>(wide.sint() >> shift);
    constant_left5 = Int<5>(wide << 32);
    constant_arithmetic17 = Int<17>(wide.sint() >> 32);
    signed_trunc5 = Int<5>(wide.sint());

    NarrowFields fields;
    fields.five = Int<5>(wide << shift);
    fields.seventeen = Int<17>(wide.sint() >> shift);
    field5 = fields.five;
    field17 = fields.seventeen;
}
