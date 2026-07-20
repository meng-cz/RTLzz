#include <fixint.hpp>
#include <array>

struct __RegProxy_uint8_t__D2__regs {
    const std::array<Int<8>, 2>& rdata;
    std::array<bool, 2>& wen;
    std::array<Int<8>, 2>& wdata;
};

#pragma input_port rdata
std::array<Int<8>, 2> rdata;
#pragma output_port wen
std::array<bool, 2> wen;
#pragma output_port wdata
std::array<Int<8>, 2> wdata;

void hls_main() {
    wen[0] = false;
    wdata[0] = rdata[0];
}
