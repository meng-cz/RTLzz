#include <uint.hpp>

#include <cstdint>
#include <iostream>
#include <string>

namespace {

template <uint32_t W>
uint32_t u32(const Int<W>& value) {
    return value.template to<uint32_t>();
}

void run_cons(uint32_t cycle_raw, uint32_t sum_raw, uint32_t d_raw, uint32_t recv_vld_raw) {
    Int<8> cycle(cycle_raw);
    Int<8> sum(sum_raw);
    Int<8> d(d_raw);
    bool recv_vld = recv_vld_raw != 0;

    bool recv_rdy = ((cycle & Int<8>(1)) == Int<8>(0));
    bool output_vld = false;
    Int<8> output_s(0);
    bool wen_sum = false;
    Int<8> wdata_sum(0);

    if (recv_rdy && recv_vld) {
        output_vld = true;
        output_s = sum;
        wen_sum = true;
        wdata_sum = Int<8>(sum + d);
    }

    bool wen_cycle = true;
    Int<8> wdata_cycle = Int<8>(cycle + Int<8>(1));

    std::cout << "cons "
              << (recv_rdy ? 1 : 0) << ' '
              << (output_vld ? 1 : 0) << ' '
              << u32(output_s) << ' '
              << (wen_sum ? 1 : 0) << ' '
              << u32(wdata_sum) << ' '
              << (wen_cycle ? 1 : 0) << ' '
              << u32(wdata_cycle) << '\n';
}

void run_intops(uint32_t a_raw, uint32_t b_raw, uint32_t sh_raw) {
    Int<8> a(a_raw);
    Int<8> b(b_raw);
    Int<4> sh(sh_raw);

    Int<9> add = a + b;
    Int<8> sub = a - b;
    Int<16> mul = a * b;

    uint8_t x = static_cast<uint8_t>(a_raw);
    uint8_t m = static_cast<uint8_t>(-(x >> 7));
    uint8_t mul2 = static_cast<uint8_t>((x << 1) ^ (0x1b & m));

    bool signed_lt = a.sint() < b.sint();
    Int<8> ashr = a.sint() >> sh;
    Int<8> lshr = a >> sh;

    Int<8> write_slice = a;
    write_slice(3, 0) = Int<4>(b_raw);

    Int<8> write_bit = a;
    write_bit(0) = static_cast<bool>(b_raw & 1U);

    auto cat = Int<4>(a_raw).cat(Int<4>(b_raw));
    auto free_cat = Cat(Int<2>(a_raw), Int<3>(b_raw), Int<1>(sh_raw));
    auto repeat = Int<2>(a_raw).repeat<4>();

    bool reduce_or = a.reduce_or();
    bool reduce_and = a.reduce_and();
    bool reduce_xor = a.reduce_xor();

    uint32_t range_idx = sh_raw % 6U;
    uint32_t bit_idx = sh_raw % 8U;
    Int<3> dyn_range = a.range_at<3>(Int<3>(range_idx));
    bool dyn_bit = a.bit_at(Int<3>(bit_idx));

    const uint8_t lut[4] = {0x11, 0x22, 0x33, 0x44};
    uint8_t lookup = lut[a_raw & 3U];

    std::cout << "intops "
              << u32(add) << ' '
              << u32(sub) << ' '
              << u32(mul) << ' '
              << static_cast<uint32_t>(mul2) << ' '
              << (signed_lt ? 1 : 0) << ' '
              << u32(ashr) << ' '
              << u32(lshr) << ' '
              << u32(write_slice) << ' '
              << u32(write_bit) << ' '
              << u32(cat) << ' '
              << u32(free_cat) << ' '
              << u32(repeat) << ' '
              << (reduce_or ? 1 : 0) << ' '
              << (reduce_and ? 1 : 0) << ' '
              << (reduce_xor ? 1 : 0) << ' '
              << u32(dyn_range) << ' '
              << (dyn_bit ? 1 : 0) << ' '
              << static_cast<uint32_t>(lookup) << '\n';
}

} // namespace

int main() {
    std::string mode;
    while (std::cin >> mode) {
        if (mode == "cons") {
            uint32_t cycle = 0, sum = 0, d = 0, recv_vld = 0;
            std::cin >> cycle >> sum >> d >> recv_vld;
            run_cons(cycle, sum, d, recv_vld);
        } else if (mode == "intops") {
            uint32_t a = 0, b = 0, sh = 0;
            std::cin >> a >> b >> sh;
            run_intops(a, b, sh);
        } else {
            return 2;
        }
    }
    return 0;
}
