#include <cstdint>
#include <fixint.hpp>

enum CoreBranchOp : uint8_t {
    CORE_BR_EQ = 0,
    CORE_BR_NE = 1,
    CORE_BR_LT = 2,
    CORE_BR_GE = 3,
    CORE_BR_LTU = 4,
    CORE_BR_GEU = 5,
};

#pragma input_port legal
bool legal;
#pragma input_port is_jal
bool is_jal;
#pragma input_port is_jalr
bool is_jalr;
#pragma input_port is_branch
bool is_branch;
#pragma input_port branch_op
uint8_t branch_op;
#pragma input_port lhs
uint64_t lhs;
#pragma input_port rhs
uint64_t rhs;
#pragma input_port imm
uint64_t imm;
#pragma input_port pc
uint64_t pc;

#pragma output_port control_valid
bool control_valid;
#pragma output_port taken
bool taken;
#pragma output_port target
Int<64> target;

void hls_main() {
    control_valid = false;
    taken = false;
    target = Int<64>(0);

    if (legal && (is_jal || is_jalr || is_branch)) {
        bool next_taken = is_jal || is_jalr;
        if (is_branch) {
            if (branch_op == CORE_BR_EQ) next_taken = lhs == rhs;
            else if (branch_op == CORE_BR_NE) next_taken = lhs != rhs;
            else if (branch_op == CORE_BR_LT)
                next_taken = static_cast<int64_t>(lhs) < static_cast<int64_t>(rhs);
            else if (branch_op == CORE_BR_GE)
                next_taken = static_cast<int64_t>(lhs) >= static_cast<int64_t>(rhs);
            else if (branch_op == CORE_BR_LTU) next_taken = lhs < rhs;
            else if (branch_op == CORE_BR_GEU) next_taken = lhs >= rhs;
        }
        uint64_t next_target = is_jalr ? ((lhs + imm) & ~1ULL) : (pc + imm);
        control_valid = true;
        taken = next_taken;
        target = Int<64>(next_target);
    }
}
