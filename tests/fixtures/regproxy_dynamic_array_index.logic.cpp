#include <array>
#include <fixint.hpp>

struct __RegProxy_uint8_t__D4__regs {
    const std::array<Int<8>, 4>& rdata;
    std::array<bool, 4>& wen;
    std::array<Int<8>, 4>& wdata;

    __RegProxy_uint8_t__D4__regs(const std::array<Int<8>, 4>& r,
                                 std::array<bool, 4>& e,
                                 std::array<Int<8>, 4>& d)
        : rdata(r), wen(e), wdata(d) {}

    uint8_t operator[](const uint32_t idx) const {
        uint8_t value;
        Int<8> raw = rdata[idx];
        value = raw.at<7, 0>();
        return value;
    }

    template <uint32_t P = 0>
    void setnext(const uint32_t idx, const uint8_t& value) {
        static_assert(P < 1, "Port index out of range");
        Int<8> packed;
        packed.at<7, 0>() = value;
        wdata[idx] = packed;
        wen[idx] = true;
    }
};

void hls_main(const std::array<Int<8>, 4>& rdata_regs__,
              std::array<bool, 4>& wen_regs__,
              std::array<Int<8>, 4>& wdata_regs__,
              const Int<2>& idx__,
              Int<8>& observed__) {
    __RegProxy_uint8_t__D4__regs regs(rdata_regs__, wen_regs__, wdata_regs__);
    uint32_t idx;
    idx = idx__.at<1, 0>();
    uint8_t old_value = regs[idx];
    observed__.at<7, 0>() = old_value;
    regs.setnext<0>(idx, old_value + uint8_t(1));
}
