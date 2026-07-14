#include <fixint.hpp>
#include <array>

struct __RegProxy_uint8_t__D2__regs {
    const std::array<Int<8>, 2>& rdata;
    std::array<bool, 2>& wen;
    std::array<Int<8>, 2>& wdata;
};

void hls_main(const std::array<Int<8>, 2>& rdata,
              std::array<bool, 2>& wen,
              std::array<Int<8>, 2>& wdata) {
    wen[0] = false;
    wdata[0] = rdata[0];
}
