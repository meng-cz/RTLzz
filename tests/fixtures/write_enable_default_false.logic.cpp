#include <fixint.hpp>

struct __RegProxy_uint8_t__sum {
    const Int<8>& rdata;
    bool& wen;
    Int<8>& wdata;

    __RegProxy_uint8_t__sum(const Int<8>& r, bool& e, Int<8>& d)
        : rdata(r), wen(e), wdata(d) {}

    template <uint32_t P = 0>
    void setnext(Int<8> value) {
        Int<8> packed;
        packed.at<7, 0>() = value;
        wdata = packed;
        wen = true;
    }
};

void hls_main(bool fire, const Int<8>& rdata_sum__, bool& wen_sum__, Int<8>& wdata_sum__) {
    __RegProxy_uint8_t__sum sum(rdata_sum__, wen_sum__, wdata_sum__);
    if (fire) {
        sum.setnext<0>(Int<8>(1));
    }
}
