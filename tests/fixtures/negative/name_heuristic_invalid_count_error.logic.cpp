#include <fixint.hpp>

void hls_main(bool fire, Int<8>& invalid_count) {
    if (fire) {
        invalid_count = Int<8>(1);
    }
}
