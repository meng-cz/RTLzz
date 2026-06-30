#include <fixint.hpp>

struct __ReqHelper__output {
    bool& vld_ports;
    Int<8>& arg_data;

    __ReqHelper__output(bool& v, Int<8>& d) : vld_ports(v), arg_data(d) {}

    void call(Int<8> value) {
        vld_ports = true;
        arg_data.at<7, 0>() = value;
    }
};

void hls_main(bool fire, bool& valid, Int<8>& payload) {
    payload = Int<8>(0);
    __ReqHelper__output output(valid, payload);
    if (fire) {
        output.call(Int<8>(7));
    }
}
