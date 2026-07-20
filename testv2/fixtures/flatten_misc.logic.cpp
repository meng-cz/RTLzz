#include <array>
#include <fixint.hpp>

using Arr2 = std::array<Int<8>, 2>;

struct Pair {
    Int<8> lo;
    Int<8> hi;
};

struct Lane {
    Pair pair;
    Arr2 taps;
};

struct Packet {
    Lane lanes[2];
    Arr2 tail;
};

struct Matrix {
    Packet packets[2];
    Pair loose[2];
};

Pair make_pair_positional(Int<8> a, Int<8> b) {
    Pair p{a, b};
    return p;
}

Pair make_pair_copy_list(Int<8> a, Int<8> b) {
    Pair p = {a ^ Int<8>(0x11), b + Int<8>(2)};
    return p;
}

Pair make_pair_designated(Int<8> a, Int<8> b) {
    Pair p{.lo = a + Int<8>(3), .hi = b ^ Int<8>(0x24)};
    return p;
}

Arr2 make_array(Int<8> a, Int<8> b) {
    Arr2 values = {a + Int<8>(5), b ^ Int<8>(0x33)};
    return values;
}

Lane make_lane(Int<8> a, Int<8> b) {
    Lane lane{
        make_pair_positional(a, b),
        make_array(a, b)
    };
    return lane;
}

Packet make_packet(Int<8> seed, Int<8> bias) {
    Packet pkt;
    pkt.lanes[0] = make_lane(seed, seed + bias);
    pkt.lanes[1] = make_lane(seed ^ Int<8>(0x55), bias + Int<8>(7));
    pkt.tail = make_array(pkt.lanes[0].pair.lo, pkt.lanes[1].pair.hi);
    return pkt;
}

Pair choose_pair(Packet pkt, int lane_idx, int tap_idx) {
    Pair selected;
    selected.lo = pkt.lanes[lane_idx].pair.lo;
    selected.hi = pkt.lanes[lane_idx].pair.hi;
    selected.lo = selected.lo + pkt.lanes[lane_idx].taps[tap_idx];
    selected.hi = selected.hi ^ pkt.tail[tap_idx];
    return selected;
}

Arr2 rotate_array(Arr2 values, int idx, Int<8> replacement) {
    Arr2 out = values;
    out[idx] = replacement;
    return out;
}

void touch_packet(Packet& pkt, int lane_idx, int tap_idx, Pair replacement) {
    pkt.lanes[lane_idx].pair.hi =
        pkt.lanes[lane_idx].pair.hi + replacement.lo;
    pkt.lanes[lane_idx].taps[tap_idx] = replacement.hi;
}

Matrix make_matrix(Packet a, Packet b, Pair p0, Pair p1) {
    Matrix mat;
    mat.packets[0] = a;
    mat.packets[1] = b;
    mat.loose[0] = p0;
    mat.loose[1] = p1;
    return mat;
}

#pragma input_port seed
Int<8> seed;
#pragma input_port bias
Int<8> bias;
#pragma input_port choose_hi
bool choose_hi;
#pragma input_port choose_tail
bool choose_tail;
#pragma output_port aggregate_pos
Int<8> aggregate_pos;
#pragma output_port aggregate_copy
Int<8> aggregate_copy;
#pragma output_port aggregate_designated
Int<8> aggregate_designated;
#pragma output_port helper_struct_return
Int<8> helper_struct_return;
#pragma output_port helper_array_return
Int<8> helper_array_return;
#pragma output_port lambda_struct_value
Int<8> lambda_struct_value;
#pragma output_port lambda_array_value
Int<8> lambda_array_value;
#pragma output_port nested_dynamic_read
Int<8> nested_dynamic_read;
#pragma output_port nested_dynamic_write
Int<8> nested_dynamic_write;
#pragma output_port internal_struct_array
Int<8> internal_struct_array;
#pragma output_port const_lookup
Int<8> const_lookup;
#pragma output_port final_mix
Int<8> final_mix;

void hls_main() {
    int lane_idx = 0;
    int tap_idx = 0;
    if (choose_hi) {
        lane_idx = 1;
    }
    if (choose_tail) {
        tap_idx = 1;
    }

    Pair positional = make_pair_positional(seed, bias);
    Pair copy_list = make_pair_copy_list(seed, bias);
    Pair designated = make_pair_designated(seed, bias);
    aggregate_pos = positional.lo + positional.hi;
    aggregate_copy = copy_list.lo ^ copy_list.hi;
    aggregate_designated = designated.lo + designated.hi;

    Packet pkt = make_packet(seed, bias);
    Pair selected = choose_pair(pkt, lane_idx, tap_idx);
    helper_struct_return = selected.lo + selected.hi;

    Arr2 array_value = make_array(selected.lo, selected.hi);
    helper_array_return = array_value[tap_idx] + pkt.tail[lane_idx];

    auto pair_lambda = [=](Pair in, Int<8> delta) -> Pair {
        Pair out = in;
        if (choose_tail) {
            out.lo = out.lo + delta;
        } else {
            out.hi = out.hi ^ delta;
        }
        return out;
    };
    Pair lambda_pair = pair_lambda(selected, bias);
    lambda_struct_value = lambda_pair.lo ^ lambda_pair.hi;

    auto array_lambda = [](Arr2 values, int idx, Int<8> patch) -> Arr2 {
        Arr2 out = values;
        out[idx] = out[idx] + patch;
        return out;
    };
    Arr2 lambda_array = array_lambda(array_value, lane_idx, seed);
    lambda_array_value = lambda_array[0] + lambda_array[1];

    Packet other = make_packet(lambda_pair.lo, lambda_pair.hi);
    Matrix mat = make_matrix(pkt, other, positional, designated);

    nested_dynamic_read =
        mat.packets[lane_idx].lanes[tap_idx].pair.lo +
        mat.packets[tap_idx].lanes[lane_idx].taps[tap_idx];

    touch_packet(mat.packets[lane_idx], tap_idx, lane_idx, lambda_pair);
    nested_dynamic_write =
        mat.packets[lane_idx].lanes[tap_idx].pair.hi ^
        mat.packets[lane_idx].lanes[tap_idx].taps[lane_idx];

    Pair locals[2];
    locals[0] = copy_list;
    locals[1] = lambda_pair;
    Int<8> local_arrays[2][2];
    local_arrays[0][0] = array_value[0];
    local_arrays[0][1] = array_value[1];
    local_arrays[1][0] = lambda_array[0];
    local_arrays[1][1] = lambda_array[1];
    internal_struct_array =
        locals[lane_idx].lo +
        locals[tap_idx].hi +
        local_arrays[lane_idx][tap_idx];

    static const Int<8> const_table[2] = {
        Int<8>(0x13),
        Int<8>(0x57),
    };
    const_lookup = const_table[tap_idx] ^ locals[lane_idx].hi;

    final_mix =
        aggregate_pos ^
        helper_struct_return ^
        nested_dynamic_read ^
        nested_dynamic_write ^
        internal_struct_array ^
        const_lookup;
}
