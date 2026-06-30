#include <fixint.hpp>

void hls_main(const bool req__vld__, Int<8>& req_out__) {
    if (req__vld__) {
        req_out__ = Int<8>(7);
    }
}
