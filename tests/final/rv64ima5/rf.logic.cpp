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

struct __RegProxy_uint64_t__D32__regs {
const std::array<Int<64>, 32> &rdata;
std::array<bool, 32> &wen;
std::array<Int<64>, 32> &wdata;

__RegProxy_uint64_t__D32__regs(const std::array<Int<64>, 32> &rdata, std::array<bool, 32> &wen, std::array<Int<64>, 32> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

uint64_t operator[](const uint32_t idx) const {
  uint64_t value;
  Int<64> rdata_val = rdata[idx];
  value = rdata_val.at<63, 0>();
  return value;
}
template <uint32_t P = 0>
void setnext(const uint32_t idx, const uint64_t &value) {
  static_assert(P < 1, "Port index out of range");
  Int<64> wdata_val;
  wdata_val.at<63, 0>() = value;
  wdata[idx] = wdata_val;
  wen[idx] = true;
}
};


void LogicSubModule_RegisterFile_top_core_rf(
  const std::array<Int<64>, 32> &rdata_regs__,
  std::array<bool, 32> &wen_regs__,
  std::array<Int<64>, 32> &wdata_regs__,
  Int<128> &zero_and_ra__query__,
  const bool write__vld__,
  const Int<32> write_rd__,
  const Int<64> write_data__,
  const Int<1> write_wen__,
  const bool read2__vld__,
  const Int<32> read2_rs1__,
  const Int<32> read2_rs2__,
  Int<128> &read2_out__
) {
__RegProxy_uint64_t__D32__regs regs(rdata_regs__, wen_regs__, wdata_regs__);

auto zero_and_ra = [&]() -> RegReadPair {
    RegReadPair out;
    out.rs1_val = 0;
    out.rs2_val = regs[1];
    return out;
};
auto write_impl__ = [&](const uint32_t & rd, const uint64_t & data, const bool & wen) -> void {
    if (wen && rd != 0U) {
        regs.setnext<0>(rd, data);
    }
};
auto read2_impl__ = [&](const uint32_t & rs1, const uint32_t & rs2, RegReadPair & out) -> void {
    out.rs1_val = (rs1 == 0U) ? 0ULL : regs[rs1];
    out.rs2_val = (rs2 == 0U) ? 0ULL : regs[rs2];
};

{
  RegReadPair value = zero_and_ra();
  Int<128> packed;
  packed.at<63, 0>() = value.rs1_val;
  packed.at<127, 64>() = value.rs2_val;
  zero_and_ra__query__ = packed;
}
{
  uint32_t rd;
  rd = write_rd__.at<31, 0>();
  uint64_t data;
  data = write_data__.at<63, 0>();
  bool wen;
  wen = write_wen__.at<0, 0>();
  if (write__vld__) {
    write_impl__(rd, data, wen);
  }
}
{
  uint32_t rs1;
  rs1 = read2_rs1__.at<31, 0>();
  uint32_t rs2;
  rs2 = read2_rs2__.at<31, 0>();
  RegReadPair out;
  if (read2__vld__) {
    read2_impl__(rs1, rs2, out);
    Int<128> out_packed;
    out_packed.at<63, 0>() = out.rs1_val;
    out_packed.at<127, 64>() = out.rs2_val;
    read2_out__ = out_packed;
  }
}

}

