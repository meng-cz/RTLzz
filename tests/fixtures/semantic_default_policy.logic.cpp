#include <cstdint>
#include <uint.hpp>

struct __RegProxy_uint8_t__state {
    const Int<8>& rdata;
    bool& wen;
    Int<8>& wdata;

    __RegProxy_uint8_t__state(const Int<8>& r, bool& e, Int<8>& d)
        : rdata(r), wen(e), wdata(d) {}

    template <uint32_t P = 0>
    void setnext(const uint8_t& value) {
        Int<8> packed;
        packed(7, 0) = value;
        wdata = packed;
        wen = true;
    }
};

void hls_main(bool fire,
              const Int<8>& rdata_state__,
              bool& commit_port__,
              Int<8>& data_port__) {
    data_port__ = Int<8>(0);
    __RegProxy_uint8_t__state state(rdata_state__, commit_port__, data_port__);
    if (fire) {
        state.setnext<0>(1);
    }
}
