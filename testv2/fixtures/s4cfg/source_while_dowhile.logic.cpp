#include <fixint.hpp>

bool keep(Int<8> x) {
    return x != Int<8>(0);
}

bool again(Int<8> x) {
    return x != Int<8>(3);
}

#pragma input_port seed
Int<8> seed;
#pragma input_port skip
bool skip;
#pragma output_port out
Int<8> out;

void hls_main() {
    Int<8> acc = seed;

    while (keep(acc)) {
        if (skip) {
            acc = Int<8>(0);
            continue;
        }
        acc = acc - Int<8>(1);
        if (acc == Int<8>(1)) {
            break;
        }
    }

    do {
        acc = acc + Int<8>(1);
        if (skip) {
            continue;
        }
    } while (again(acc));

    out = acc;
}
