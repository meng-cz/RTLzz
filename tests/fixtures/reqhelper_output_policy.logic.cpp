#include <cstdint>
#include <fixint.hpp>

struct __ReqHelper__output {
    bool& vld_ports;
    Int<8>& arg_s;

    __ReqHelper__output(bool& v, Int<8>& s) : vld_ports(v), arg_s(s) {}

    void call(uint8_t value) {
        vld_ports = true;
        arg_s.at<7, 0>() = value;
    }
};

void hls_main(bool fire, uint8_t payload, bool& output__vld__, Int<8>& output_s__) {
    output_s__ = Int<8>(0);
    __ReqHelper__output output(output__vld__, output_s__);
    if (fire) {
        output.call(payload);
    }
}
