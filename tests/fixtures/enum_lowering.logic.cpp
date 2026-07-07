#include <cstdint>
#include <fixint.hpp>

enum class Small : uint8_t {
    Zero,
    Three = 3,
    Four,
    Nine = 9,
};

enum class Mirror : uint8_t {
    One = 1,
    Two,
    Six = 6,
    Seven,
};

void hls_main(Int<8> code,
              bool sel,
              Int<8>& zero_value,
              Int<8>& four_value,
              Int<8>& nine_value,
              Int<8>& selected_value,
              Int<8>& roundtrip_value,
              Int<8>& std_value,
              Int<8>& compare_value,
              Int<8>& mirror_value) {
    Small selected = Small::Three;
    if (sel) {
        selected = Small::Four;
    }

    uint8_t code_raw = code.template to<uint8_t>();
    Small from_code = static_cast<Small>(code_raw);

    uint8_t std_raw = sel ? static_cast<uint8_t>(9) : static_cast<uint8_t>(4);
    Small from_std = static_cast<Small>(std_raw);

    Mirror mirror = sel ? Mirror::Seven : Mirror::Two;

    zero_value = static_cast<uint8_t>(Small::Zero);
    four_value = static_cast<uint8_t>(Small::Four);
    nine_value = static_cast<uint8_t>(Small::Nine);
    selected_value = static_cast<uint8_t>(selected);
    roundtrip_value = static_cast<uint8_t>(from_code);
    std_value = static_cast<uint8_t>(from_std);
    compare_value = (selected == Small::Four) ? Int<8>(1) : Int<8>(0);
    mirror_value = static_cast<uint8_t>(mirror);
}
