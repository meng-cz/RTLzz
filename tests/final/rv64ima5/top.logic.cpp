#include <array>
#include <cstdint>

using std::array;

using int8 = int8_t;
using uint8 = uint8_t;
using int16 = int16_t;
using uint16 = uint16_t;
using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;

#define assert(...) ((void)0)

constexpr int64_t ALU_ADD = 0;
constexpr int64_t ALU_AND = 9;
constexpr int64_t ALU_COPY_RS1 = 10;
constexpr int64_t ALU_COPY_RS2 = 11;
constexpr int64_t ALU_OR = 8;
constexpr int64_t ALU_SLL = 2;
constexpr int64_t ALU_SLT = 3;
constexpr int64_t ALU_SLTU = 4;
constexpr int64_t ALU_SRA = 7;
constexpr int64_t ALU_SRL = 6;
constexpr int64_t ALU_SUB = 1;
constexpr int64_t ALU_XOR = 5;
constexpr int64_t AMO_ADD = 2;
constexpr int64_t AMO_AND = 4;
constexpr int64_t AMO_MAX = 7;
constexpr int64_t AMO_MAXU = 9;
constexpr int64_t AMO_MIN = 6;
constexpr int64_t AMO_MINU = 8;
constexpr int64_t AMO_NONE = 0;
constexpr int64_t AMO_OR = 5;
constexpr int64_t AMO_SWAP = 1;
constexpr int64_t AMO_XOR = 3;
constexpr int64_t BR_EQ = 1;
constexpr int64_t BR_GE = 4;
constexpr int64_t BR_GEU = 6;
constexpr int64_t BR_LT = 3;
constexpr int64_t BR_LTU = 5;
constexpr int64_t BR_NE = 2;
constexpr int64_t BR_NONE = 0;
constexpr int64_t MDU_DIV = 5;
constexpr int64_t MDU_DIVU = 6;
constexpr int64_t MDU_MUL = 1;
constexpr int64_t MDU_MULH = 2;
constexpr int64_t MDU_MULHSU = 3;
constexpr int64_t MDU_MULHU = 4;
constexpr int64_t MDU_NONE = 0;
constexpr int64_t MDU_REM = 7;
constexpr int64_t MDU_REMU = 8;

struct RegReadPair {
    uint64_t rs1_val;
    uint64_t rs2_val;
};

struct DecodedInst {
    bool valid;
    bool writes_rd;
    bool use_rs1;
    bool use_rs2;
    bool is_load;
    bool is_store;
    bool is_branch;
    bool is_jal;
    bool is_jalr;
    bool is_lui;
    bool is_auipc;
    bool is_ebreak;
    bool is_muldiv;
    bool is_atomic;
    bool is_lr;
    bool is_sc;
    uint8_t rd;
    uint8_t rs1;
    uint8_t rs2;
    uint8_t alu_op;
    uint8_t branch_op;
    uint8_t mem_width;
    bool mem_unsigned;
    uint8_t muldiv_op;
    uint8_t atomic_op;
    uint32_t inst;
    uint64_t imm;
};

struct ExecRequest {
    uint64_t pc;
    uint64_t rs1_val;
    uint64_t rs2_val;
    DecodedInst dec;
};

struct ExecResult {
    bool valid;
    bool branch_taken;
    uint64_t branch_target;
    uint64_t result;
    uint64_t mem_addr;
    uint64_t store_data;
};

struct MemRequest {
    bool valid;
    bool is_write;
    bool is_atomic;
    bool is_lr;
    bool is_sc;
    bool mem_unsigned;
    uint8_t width;
    uint8_t atomic_op;
    uint64_t addr;
    uint64_t wdata;
};

struct MemResponse {
    uint64_t rdata;
    bool success;
};

struct IfIdStage {
    bool valid;
    uint64_t pc;
    uint32_t inst;
};

struct IdExStage {
    bool valid;
    uint64_t pc;
    uint64_t rs1_val;
    uint64_t rs2_val;
    DecodedInst dec;
};

struct ExMemStage {
    bool valid;
    uint64_t pc;
    ExecResult ex;
    DecodedInst dec;
};

struct MemWbStage {
    bool valid;
    uint64_t pc;
    uint32_t inst;
    bool writes_rd;
    uint8_t rd;
    uint64_t wb_data;
    bool halt;
};

struct CoreStatus {
    uint64_t cycle;
    uint64_t pc;
    bool halted;
    bool if_valid;
    bool id_valid;
    bool ex_valid;
    bool mem_valid;
    bool wb_valid;
};

struct __ChildQueryHelper__core_status {
  const Int<134> & value_0;

  CoreStatus call() const {
    CoreStatus value;
    value.cycle = value_0.at<63, 0>();
    value.pc = value_0.at<127, 64>();
    value.halted = value_0.at<128, 128>();
    value.if_valid = value_0.at<129, 129>();
    value.id_valid = value_0.at<130, 130>();
    value.ex_valid = value_0.at<131, 131>();
    value.mem_valid = value_0.at<132, 132>();
    value.wb_valid = value_0.at<133, 133>();
    return value;
  }
};

void LogicSubModule_Top_top(
  Int<134> &status__query__,
  const Int<134> core_status__query__
) {
__ChildQueryHelper__core_status __child_query_helper__core_status{
  core_status__query__,
};
#define core_status __child_query_helper__core_status.call

auto status = [&]() -> CoreStatus {
    return core_status();
};

{
  CoreStatus value = status();
  Int<134> packed;
  packed.at<63, 0>() = value.cycle;
  packed.at<127, 64>() = value.pc;
  packed.at<128, 128>() = value.halted;
  packed.at<129, 129>() = value.if_valid;
  packed.at<130, 130>() = value.id_valid;
  packed.at<131, 131>() = value.ex_valid;
  packed.at<132, 132>() = value.mem_valid;
  packed.at<133, 133>() = value.wb_valid;
  status__query__ = packed;
}

#undef core_status
}

