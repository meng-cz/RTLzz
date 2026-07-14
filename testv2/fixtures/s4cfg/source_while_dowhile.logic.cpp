#include <fixint.hpp>

bool keep(Int<8> x) {
    return x != Int<8>(0);
}

bool again(Int<8> x) {
    return x != Int<8>(3);
}

void hls_main(Int<8> seed, bool skip, Int<8>& out) {
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
