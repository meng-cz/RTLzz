#include <array>
#include <cstdint>
#include <fixint.hpp>

#pragma input_port register_file
std::array<Int<64>, 32> register_file;

#pragma output_port snapshot
std::array<Int<64>, 32> snapshot;

Int<64> read_register_inner(uint32_t index) {
    return register_file[index];
}

Int<64> read_register(uint32_t index) {
    return read_register_inner(index);
}

void hls_main() {
    snapshot[0] = read_register(0);
    snapshot[1] = read_register(1);
    snapshot[2] = read_register(2);
    snapshot[3] = read_register(3);
    snapshot[4] = read_register(4);
    snapshot[5] = read_register(5);
    snapshot[6] = read_register(6);
    snapshot[7] = read_register(7);
    snapshot[8] = read_register(8);
    snapshot[9] = read_register(9);
    snapshot[10] = read_register(10);
    snapshot[11] = read_register(11);
    snapshot[12] = read_register(12);
    snapshot[13] = read_register(13);
    snapshot[14] = read_register(14);
    snapshot[15] = read_register(15);
    snapshot[16] = read_register(16);
    snapshot[17] = read_register(17);
    snapshot[18] = read_register(18);
    snapshot[19] = read_register(19);
    snapshot[20] = read_register(20);
    snapshot[21] = read_register(21);
    snapshot[22] = read_register(22);
    snapshot[23] = read_register(23);
    snapshot[24] = read_register(24);
    snapshot[25] = read_register(25);
    snapshot[26] = read_register(26);
    snapshot[27] = read_register(27);
    snapshot[28] = read_register(28);
    snapshot[29] = read_register(29);
    snapshot[30] = read_register(30);
    snapshot[31] = read_register(31);
}
