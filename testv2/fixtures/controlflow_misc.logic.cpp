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

// Integrated regression from backend_structure.logic.cpp
#pragma input_port structure_selector
Int<3> structure_selector;
#pragma input_port structure_a
Int<8> structure_a;
#pragma input_port structure_b
Int<8> structure_b;
#pragma input_port structure_c
Int<8> structure_c;
#pragma input_port structure_d
Int<8> structure_d;
#pragma input_port structure_wide
Int<128> structure_wide;
#pragma output_port structure_wide_mux
Int<128> structure_wide_mux;
#pragma output_port structure_exclusive
Int<8> structure_exclusive;
#pragma output_port structure_priority_result
Int<8> structure_priority_result;
#pragma output_port structure_tree_xor
Int<8> structure_tree_xor;
#pragma output_port structure_tree_add
Int<8> structure_tree_add;
#pragma output_port structure_nested
Int<8> structure_nested;
// Integrated regression from partial_predicate.logic.cpp
#pragma input_port structure_partial_predicate_a
bool structure_partial_predicate_a;
#pragma input_port structure_partial_predicate_b
bool structure_partial_predicate_b;
#pragma input_port structure_partial_predicate_c
bool structure_partial_predicate_c;
#pragma input_port structure_partial_predicate_x
Int<8> structure_partial_predicate_x;
#pragma input_port structure_partial_predicate_y
Int<8> structure_partial_predicate_y;
#pragma output_port structure_partial_predicate_direct
Int<8> structure_partial_predicate_direct;
#pragma output_port structure_partial_predicate_inverse
Int<8> structure_partial_predicate_inverse;
#pragma output_port structure_partial_predicate_deep
Int<8> structure_partial_predicate_deep;
#pragma output_port structure_partial_predicate_shared
bool structure_partial_predicate_shared;
#pragma output_port structure_partial_predicate_unrestricted
Int<8> structure_partial_predicate_unrestricted;

// Integrated regression from predicate_chain.logic.cpp
#pragma input_port structure_predicate_chain_state
Int<8> structure_predicate_chain_state;
#pragma input_port structure_predicate_chain_carry
bool structure_predicate_chain_carry;
#pragma input_port structure_predicate_chain_fallback
bool structure_predicate_chain_fallback;
#pragma output_port structure_predicate_chain_result
bool structure_predicate_chain_result;
#pragma output_port structure_predicate_chain_shared_inner
bool structure_predicate_chain_shared_inner;
#pragma output_port structure_predicate_chain_inverse
bool structure_predicate_chain_inverse;

// Integrated regression from branch_decision.logic.cpp
enum CoreBranchOp : uint8_t {
    CORE_BR_EQ = 0,
    CORE_BR_NE = 1,
    CORE_BR_LT = 2,
    CORE_BR_GE = 3,
    CORE_BR_LTU = 4,
    CORE_BR_GEU = 5,
};

#pragma input_port branch_legal
bool branch_legal;
#pragma input_port branch_is_jal
bool branch_is_jal;
#pragma input_port branch_is_jalr
bool branch_is_jalr;
#pragma input_port branch_is_branch
bool branch_is_branch;
#pragma input_port branch_branch_op
uint8_t branch_branch_op;
#pragma input_port branch_lhs
uint64_t branch_lhs;
#pragma input_port branch_rhs
uint64_t branch_rhs;
#pragma input_port branch_imm
uint64_t branch_imm;
#pragma input_port branch_pc
uint64_t branch_pc;

#pragma output_port branch_control_valid
bool branch_control_valid;
#pragma output_port branch_taken
bool branch_taken;
#pragma output_port branch_target
Int<64> branch_target;

// Integrated regression from shortcircuit_pure.logic.cpp
#pragma input_port shortcircuit_gate
bool shortcircuit_gate;
#pragma input_port shortcircuit_a
bool shortcircuit_a;
#pragma input_port shortcircuit_b
bool shortcircuit_b;
#pragma input_port shortcircuit_lookup_slot
uint8_t shortcircuit_lookup_slot;
#pragma output_port shortcircuit_selected
bool shortcircuit_selected;
#pragma output_port shortcircuit_or_result
bool shortcircuit_or_result;
#pragma output_port shortcircuit_and_result
bool shortcircuit_and_result;
#pragma output_port shortcircuit_or_calls
bool shortcircuit_or_calls;
#pragma output_port shortcircuit_and_calls
bool shortcircuit_and_calls;
#pragma output_port shortcircuit_protected_lookup
bool shortcircuit_protected_lookup;

bool shortcircuit_effect_or() { shortcircuit_or_calls = true; return shortcircuit_b; }
bool shortcircuit_effect_and() { shortcircuit_and_calls = true; return shortcircuit_b; }

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

    // backend_structure.logic.cpp
    {
    structure_exclusive = structure_selector == Int<3>(0) ? structure_a :
                structure_selector == Int<3>(1) ? structure_b :
                structure_selector == Int<3>(2) ? structure_c :
                structure_selector == Int<3>(3) ? structure_d : Int<8>(93);
    structure_wide_mux = structure_selector == Int<3>(0) ? structure_wide :
               structure_selector == Int<3>(1) ? ~structure_wide :
               structure_selector == Int<3>(2) ? Int<128>(structure_a) : Int<128>(93);
    structure_priority_result = (structure_a != Int<8>(0)) ? structure_b : (structure_b != Int<8>(0)) ? structure_c : structure_d;
    structure_tree_xor = (((((structure_a ^ structure_b) ^ structure_c) ^ structure_d) ^ Int<8>(17)) ^ Int<8>(93));
    Int<8> sum = Int<8>(structure_a + structure_b);
    sum = Int<8>(sum + structure_c);
    sum = Int<8>(sum + structure_d);
    sum = Int<8>(sum + Int<8>(17));
    structure_tree_add = sum;
    Int<8> inner = ((structure_selector == Int<3>(0)) && (structure_a != Int<8>(0))) ? structure_b : structure_c;
    structure_nested = structure_selector == Int<3>(0) ? (structure_a != Int<8>(0) ? inner : structure_d) : structure_a;

    // partial_predicate.logic.cpp
    {
    bool ab = structure_partial_predicate_a && structure_partial_predicate_b;
    Int<8> v = ab ? structure_partial_predicate_x : structure_partial_predicate_y;
    Int<8> n = !ab ? structure_partial_predicate_x : structure_partial_predicate_y;
    Int<8> structure_d = (ab && structure_partial_predicate_c) ? structure_partial_predicate_x : structure_partial_predicate_y;
    structure_partial_predicate_direct = structure_partial_predicate_a ? v : Int<8>(0);
    structure_partial_predicate_inverse = structure_partial_predicate_a ? n : Int<8>(0);
    structure_partial_predicate_deep = structure_partial_predicate_a ? structure_d : Int<8>(0);
    structure_partial_predicate_shared = ab;
    structure_partial_predicate_unrestricted = v;
    }

    // predicate_chain.logic.cpp
    {
    bool prefix = structure_predicate_chain_state != 0 && structure_predicate_chain_state != 1 && structure_predicate_chain_state != 2 && structure_predicate_chain_state != 3 &&
        structure_predicate_chain_state != 4 && structure_predicate_chain_state != 5 && structure_predicate_chain_state != 6 && structure_predicate_chain_state != 7 && structure_predicate_chain_state != 8 &&
        structure_predicate_chain_state != 9 && structure_predicate_chain_state != 10 && structure_predicate_chain_state != 11 && structure_predicate_chain_state != 12 && structure_predicate_chain_state != 13 &&
        structure_predicate_chain_state != 14 && structure_predicate_chain_state != 15 && structure_predicate_chain_state != 16 && structure_predicate_chain_state != 17 && structure_predicate_chain_state != 18 &&
        structure_predicate_chain_state != 19 && structure_predicate_chain_state != 20 && structure_predicate_chain_state != 21 && structure_predicate_chain_state != 22 && structure_predicate_chain_state != 23 && structure_predicate_chain_state != 24;
    bool inner = (!prefix || structure_predicate_chain_state == 25) ? structure_predicate_chain_carry : structure_predicate_chain_fallback;
    structure_predicate_chain_result = prefix ? inner : structure_predicate_chain_fallback;
    structure_predicate_chain_shared_inner = inner;
    structure_predicate_chain_inverse = !(!prefix ? structure_predicate_chain_fallback : inner);
    }
    }

    // branch_decision.logic.cpp
    {
    branch_control_valid = false;
    branch_taken = false;
    branch_target = Int<64>(0);

    if (branch_legal && (branch_is_jal || branch_is_jalr || branch_is_branch)) {
        bool next_taken = branch_is_jal || branch_is_jalr;
        if (branch_is_branch) {
            if (branch_branch_op == CORE_BR_EQ) next_taken = branch_lhs == branch_rhs;
            else if (branch_branch_op == CORE_BR_NE) next_taken = branch_lhs != branch_rhs;
            else if (branch_branch_op == CORE_BR_LT)
                next_taken = static_cast<int64_t>(branch_lhs) < static_cast<int64_t>(branch_rhs);
            else if (branch_branch_op == CORE_BR_GE)
                next_taken = static_cast<int64_t>(branch_lhs) >= static_cast<int64_t>(branch_rhs);
            else if (branch_branch_op == CORE_BR_LTU) next_taken = branch_lhs < branch_rhs;
            else if (branch_branch_op == CORE_BR_GEU) next_taken = branch_lhs >= branch_rhs;
        }
        uint64_t next_target = branch_is_jalr ? ((branch_lhs + branch_imm) & ~1ULL) : (branch_pc + branch_imm);
        branch_control_valid = true;
        branch_taken = next_taken;
        branch_target = Int<64>(next_target);
    }
    }

    // shortcircuit_pure.logic.cpp
    {
    shortcircuit_selected = 0;
    shortcircuit_or_calls = 0;
    shortcircuit_and_calls = 0;
    if (shortcircuit_gate) {
        bool local = shortcircuit_a || (shortcircuit_b && !shortcircuit_a);
        shortcircuit_selected = local;
    }
    shortcircuit_or_result = shortcircuit_a || shortcircuit_effect_or();
    shortcircuit_and_result = shortcircuit_a && shortcircuit_effect_and();
    const uint8_t table[4] = {1, 2, 0, 0};
    shortcircuit_protected_lookup = shortcircuit_lookup_slot >= 4U || table[shortcircuit_lookup_slot] != 0U;
    }
}
