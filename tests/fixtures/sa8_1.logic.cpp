#include <array>
#include <cstdint>

using std::array;

void hls_main(
    const uint16_t ALeft,
    const bool AInitLeft,
    const uint16_t BTop,

    uint16_t * ARight,
    bool * AInitRight,
    uint16_t * BBottom,

    uint16_t * CRegOut,

    const uint16_t A_reg_in,
    const bool AInit_reg_in,
    const bool B_reg_in,
    const uint16_t Sum_reg_in,

    uint16_t * A_reg_out,
    uint16_t * AInit_reg_out,
    uint16_t * B_reg_out,
    uint16_t * Sum_reg_out
) {
    *A_reg_out = ALeft;
    *AInit_reg_out = AInitLeft;
    *B_reg_out = BTop;

    *ARight = A_reg_in;
    *AInitRight = AInit_reg_in;
    *BBottom = B_reg_in;

    uint16_t mulres = A_reg_in * B_reg_in;
    uint16_t sumres = mulres + (AInit_reg_in ? 0 : Sum_reg_in);
    *Sum_reg_out = sumres;

    *CRegOut = Sum_reg_in;
}
