#include <cstdint>
#include <fixint.hpp>

Int<8> early_helper(Int<8> seed, bool gate, bool sel, bool alt) {
    Int<8> acc = seed + Int<8>(1);
    if (gate) {
        if (sel) {
            return acc ^ Int<8>(0x31);
        }
        acc = acc + Int<8>(2);
        if (alt) {
            return acc ^ Int<8>(0x42);
        }
    } else {
        if (alt) {
            return acc + Int<8>(5);
        }
    }
    return acc ^ Int<8>(0x13);
}

Int<8> nested_switch_helper(Int<8> base, uint8_t outer, uint8_t inner) {
    Int<8> acc = base;
    switch (outer) {
    case 0:
        acc = acc + Int<8>(1);
        switch (inner) {
        case 0:
            acc = acc ^ Int<8>(0x11);
            break;
        case 1:
            acc = acc + Int<8>(2);
            break;
        default:
            acc = acc ^ Int<8>(0x22);
            break;
        }
        break;
    case 1:
        acc = acc + Int<8>(3);
        switch (inner) {
        case 0:
            acc = acc ^ Int<8>(0x33);
            break;
        case 2:
            acc = acc + Int<8>(4);
            break;
        default:
            acc = acc ^ Int<8>(0x44);
            break;
        }
        break;
    default:
        acc = acc + Int<8>(5);
        acc = acc + Int<8>(6);
        break;
    }
    return acc;
}

Int<8> loop_control_helper(Int<8> seed,
                           bool skip_outer,
                           bool stop_outer,
                           bool skip_inner,
                           bool stop_inner,
                           uint8_t mode) {
    Int<8> acc = seed;
    for (uint32_t i = 0; i < 3; ++i) {
        if (skip_outer) {
            continue;
        }
        if (stop_outer) {
            break;
        }
        for (uint32_t j = 0; j < 4; ++j) {
            if (skip_inner) {
                continue;
            }
            if (stop_inner) {
                break;
            }
            acc = acc + Int<8>(1);
        }
        switch (mode) {
        case 0:
            acc = acc ^ Int<8>(0x12);
            break;
        case 1:
            acc = acc + Int<8>(2);
            break;
        default:
            acc = acc ^ Int<8>(0x23);
            break;
        }
    }
    return acc;
}

#pragma input_port seed
Int<8> seed;
#pragma input_port mode
uint8_t mode;
#pragma input_port a
bool a;
#pragma input_port b
bool b;
#pragma input_port c
bool c;
#pragma input_port early_top
bool early_top;
#pragma output_port if_value
Int<8> if_value;
#pragma output_port switch_value
Int<8> switch_value;
#pragma output_port loop_value
Int<8> loop_value;
#pragma output_port early_value
Int<8> early_value;
#pragma output_port final_value
Int<8> final_value;

void hls_main() {
    if_value = Int<8>(0);
    switch_value = Int<8>(0);
    loop_value = Int<8>(0);
    early_value = Int<8>(0);
    final_value = Int<8>(0);

    Int<8> if_acc = seed;
    if (a) {
        if_acc = if_acc + Int<8>(1);
        if (b) {
            if_acc = if_acc ^ Int<8>(0x21);
            if (c) {
                if_acc = if_acc + Int<8>(3);
            } else {
                if_acc = if_acc ^ Int<8>(0x32);
            }
        } else {
            if_acc = if_acc + Int<8>(4);
            if (c) {
                if_acc = if_acc ^ Int<8>(0x43);
            }
        }
    } else {
        if_acc = if_acc ^ Int<8>(0x54);
        if (b) {
            if_acc = if_acc + Int<8>(5);
        } else if (c) {
            if_acc = if_acc ^ Int<8>(0x65);
        } else {
            if_acc = if_acc + Int<8>(6);
        }
    }
    if_value = if_acc;

    switch_value = if_acc ^ Int<8>(0x5a);

    Int<8> loop_acc = seed;
    for (uint32_t i = 0; i < 3; ++i) {
        for (uint32_t j = 0; j < 4; ++j) {
            loop_acc = loop_acc + Int<8>(3);
        }
    }
    loop_value = loop_acc;

    early_value = early_helper(loop_acc, b, a, c);

    if (early_top) {
        final_value = if_value ^ early_value;
    } else {
        final_value = if_value ^ switch_value ^ loop_value ^ early_value;
    }
}
