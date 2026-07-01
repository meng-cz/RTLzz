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

inline uint64_t sign_extend_u64(uint64_t value, uint32_t bits) {
    if (bits >= 64) {
        return value;
    }
    uint64_t mask = (1ULL << bits) - 1ULL;
    value &= mask;
    uint64_t sign_bit = 1ULL << (bits - 1);
    if ((value & sign_bit) != 0) {
        value |= ~mask;
    }
    return value;
}


void LogicSubModule_Decoder_top_core_dec(
  const bool decode__vld__,
  const Int<32> decode_inst__,
  Int<177> &decode_out__
) {
auto decode_impl__ = [&](const uint32_t & inst, DecodedInst & out) -> void {
    uint32_t opcode = inst & 0x7fU;
    uint32_t rd = (inst >> 7) & 0x1fU;
    uint32_t funct3 = (inst >> 12) & 0x7U;
    uint32_t rs1 = (inst >> 15) & 0x1fU;
    uint32_t rs2 = (inst >> 20) & 0x1fU;
    uint32_t funct7 = (inst >> 25) & 0x7fU;
    uint32_t funct5 = (inst >> 27) & 0x1fU;
    out.valid = true;
    out.writes_rd = false;
    out.use_rs1 = false;
    out.use_rs2 = false;
    out.is_load = false;
    out.is_store = false;
    out.is_branch = false;
    out.is_jal = false;
    out.is_jalr = false;
    out.is_lui = false;
    out.is_auipc = false;
    out.is_ebreak = false;
    out.is_muldiv = false;
    out.is_atomic = false;
    out.is_lr = false;
    out.is_sc = false;
    out.rd = static_cast<uint8_t>(rd);
    out.rs1 = static_cast<uint8_t>(rs1);
    out.rs2 = static_cast<uint8_t>(rs2);
    out.alu_op = static_cast<uint8_t>(ALU_ADD);
    out.branch_op = static_cast<uint8_t>(BR_NONE);
    out.mem_width = 8;
    out.mem_unsigned = false;
    out.muldiv_op = static_cast<uint8_t>(MDU_NONE);
    out.atomic_op = static_cast<uint8_t>(AMO_NONE);
    out.inst = inst;
    out.imm = 0;
    if (opcode == 0x37U) {
        out.is_lui = true;
        out.writes_rd = true;
        out.imm = sign_extend_u64(static_cast<uint64_t>(inst & 0xfffff000U), 32);
    } else if (opcode == 0x17U) {
        out.is_auipc = true;
        out.writes_rd = true;
        out.imm = sign_extend_u64(static_cast<uint64_t>(inst & 0xfffff000U), 32);
    } else if (opcode == 0x6fU) {
        uint64_t imm = 0;
        imm |= ((static_cast<uint64_t>(inst) >> 31) & 0x1ULL) << 20;
        imm |= ((static_cast<uint64_t>(inst) >> 12) & 0xffULL) << 12;
        imm |= ((static_cast<uint64_t>(inst) >> 20) & 0x1ULL) << 11;
        imm |= ((static_cast<uint64_t>(inst) >> 21) & 0x3ffULL) << 1;
        out.is_jal = true;
        out.writes_rd = true;
        out.imm = sign_extend_u64(imm, 21);
    } else if (opcode == 0x67U && funct3 == 0U) {
        out.is_jalr = true;
        out.writes_rd = true;
        out.use_rs1 = true;
        out.imm = sign_extend_u64(static_cast<uint64_t>(inst >> 20), 12);
    } else if (opcode == 0x63U) {
        uint64_t imm = 0;
        imm |= ((static_cast<uint64_t>(inst) >> 31) & 0x1ULL) << 12;
        imm |= ((static_cast<uint64_t>(inst) >> 7) & 0x1ULL) << 11;
        imm |= ((static_cast<uint64_t>(inst) >> 25) & 0x3fULL) << 5;
        imm |= ((static_cast<uint64_t>(inst) >> 8) & 0xfULL) << 1;
        out.is_branch = true;
        out.use_rs1 = true;
        out.use_rs2 = true;
        out.imm = sign_extend_u64(imm, 13);
        if (funct3 == 0U) out.branch_op = static_cast<uint8_t>(BR_EQ);
        else if (funct3 == 1U) out.branch_op = static_cast<uint8_t>(BR_NE);
        else if (funct3 == 4U) out.branch_op = static_cast<uint8_t>(BR_LT);
        else if (funct3 == 5U) out.branch_op = static_cast<uint8_t>(BR_GE);
        else if (funct3 == 6U) out.branch_op = static_cast<uint8_t>(BR_LTU);
        else if (funct3 == 7U) out.branch_op = static_cast<uint8_t>(BR_GEU);
        else out.valid = false;
    } else if (opcode == 0x03U) {
        out.is_load = true;
        out.writes_rd = true;
        out.use_rs1 = true;
        out.imm = sign_extend_u64(static_cast<uint64_t>(inst >> 20), 12);
        if (funct3 == 0U) {
            out.mem_width = 1;
        } else if (funct3 == 1U) {
            out.mem_width = 2;
        } else if (funct3 == 2U) {
            out.mem_width = 4;
        } else if (funct3 == 3U) {
            out.mem_width = 8;
        } else if (funct3 == 4U) {
            out.mem_width = 1;
            out.mem_unsigned = true;
        } else if (funct3 == 5U) {
            out.mem_width = 2;
            out.mem_unsigned = true;
        } else if (funct3 == 6U) {
            out.mem_width = 4;
            out.mem_unsigned = true;
        } else {
            out.valid = false;
        }
    } else if (opcode == 0x23U) {
        uint64_t imm = ((static_cast<uint64_t>(inst >> 25) & 0x7fULL) << 5) |
                       ((static_cast<uint64_t>(inst >> 7) & 0x1fULL));
        out.is_store = true;
        out.use_rs1 = true;
        out.use_rs2 = true;
        out.imm = sign_extend_u64(imm, 12);
        if (funct3 == 0U) out.mem_width = 1;
        else if (funct3 == 1U) out.mem_width = 2;
        else if (funct3 == 2U) out.mem_width = 4;
        else if (funct3 == 3U) out.mem_width = 8;
        else out.valid = false;
    } else if (opcode == 0x13U) {
        out.writes_rd = true;
        out.use_rs1 = true;
        out.imm = sign_extend_u64(static_cast<uint64_t>(inst >> 20), 12);
        if (funct3 == 0U) out.alu_op = static_cast<uint8_t>(ALU_ADD);
        else if (funct3 == 2U) out.alu_op = static_cast<uint8_t>(ALU_SLT);
        else if (funct3 == 3U) out.alu_op = static_cast<uint8_t>(ALU_SLTU);
        else if (funct3 == 4U) out.alu_op = static_cast<uint8_t>(ALU_XOR);
        else if (funct3 == 6U) out.alu_op = static_cast<uint8_t>(ALU_OR);
        else if (funct3 == 7U) out.alu_op = static_cast<uint8_t>(ALU_AND);
        else if (funct3 == 1U && funct7 == 0U) {
            out.alu_op = static_cast<uint8_t>(ALU_SLL);
            out.imm = static_cast<uint64_t>((inst >> 20) & 0x3fU);
        } else if (funct3 == 5U && funct7 == 0U) {
            out.alu_op = static_cast<uint8_t>(ALU_SRL);
            out.imm = static_cast<uint64_t>((inst >> 20) & 0x3fU);
        } else if (funct3 == 5U && funct7 == 0x20U) {
            out.alu_op = static_cast<uint8_t>(ALU_SRA);
            out.imm = static_cast<uint64_t>((inst >> 20) & 0x3fU);
        } else {
            out.valid = false;
        }
    } else if (opcode == 0x33U) {
        out.writes_rd = true;
        out.use_rs1 = true;
        out.use_rs2 = true;
        if (funct7 == 0x00U) {
            if (funct3 == 0U) out.alu_op = static_cast<uint8_t>(ALU_ADD);
            else if (funct3 == 1U) out.alu_op = static_cast<uint8_t>(ALU_SLL);
            else if (funct3 == 2U) out.alu_op = static_cast<uint8_t>(ALU_SLT);
            else if (funct3 == 3U) out.alu_op = static_cast<uint8_t>(ALU_SLTU);
            else if (funct3 == 4U) out.alu_op = static_cast<uint8_t>(ALU_XOR);
            else if (funct3 == 5U) out.alu_op = static_cast<uint8_t>(ALU_SRL);
            else if (funct3 == 6U) out.alu_op = static_cast<uint8_t>(ALU_OR);
            else if (funct3 == 7U) out.alu_op = static_cast<uint8_t>(ALU_AND);
            else out.valid = false;
        } else if (funct7 == 0x20U) {
            if (funct3 == 0U) out.alu_op = static_cast<uint8_t>(ALU_SUB);
            else if (funct3 == 5U) out.alu_op = static_cast<uint8_t>(ALU_SRA);
            else out.valid = false;
        } else if (funct7 == 0x01U) {
            out.is_muldiv = true;
            if (funct3 == 0U) out.muldiv_op = static_cast<uint8_t>(MDU_MUL);
            else if (funct3 == 1U) out.muldiv_op = static_cast<uint8_t>(MDU_MULH);
            else if (funct3 == 2U) out.muldiv_op = static_cast<uint8_t>(MDU_MULHSU);
            else if (funct3 == 3U) out.muldiv_op = static_cast<uint8_t>(MDU_MULHU);
            else if (funct3 == 4U) out.muldiv_op = static_cast<uint8_t>(MDU_DIV);
            else if (funct3 == 5U) out.muldiv_op = static_cast<uint8_t>(MDU_DIVU);
            else if (funct3 == 6U) out.muldiv_op = static_cast<uint8_t>(MDU_REM);
            else if (funct3 == 7U) out.muldiv_op = static_cast<uint8_t>(MDU_REMU);
            else out.valid = false;
        } else {
            out.valid = false;
        }
    } else if (opcode == 0x2fU) {
        out.is_atomic = true;
        out.writes_rd = true;
        out.use_rs1 = true;
        out.use_rs2 = true;
        out.mem_width = (funct3 == 2U) ? 4 : 8;
        if (funct3 != 2U && funct3 != 3U) {
            out.valid = false;
        }
        if (funct5 == 0x02U) {
            out.is_lr = true;
            out.use_rs2 = false;
        } else if (funct5 == 0x03U) {
            out.is_sc = true;
        } else if (funct5 == 0x01U) {
            out.atomic_op = static_cast<uint8_t>(AMO_SWAP);
        } else if (funct5 == 0x00U) {
            out.atomic_op = static_cast<uint8_t>(AMO_ADD);
        } else if (funct5 == 0x04U) {
            out.atomic_op = static_cast<uint8_t>(AMO_XOR);
        } else if (funct5 == 0x0cU) {
            out.atomic_op = static_cast<uint8_t>(AMO_AND);
        } else if (funct5 == 0x08U) {
            out.atomic_op = static_cast<uint8_t>(AMO_OR);
        } else if (funct5 == 0x10U) {
            out.atomic_op = static_cast<uint8_t>(AMO_MIN);
        } else if (funct5 == 0x14U) {
            out.atomic_op = static_cast<uint8_t>(AMO_MAX);
        } else if (funct5 == 0x18U) {
            out.atomic_op = static_cast<uint8_t>(AMO_MINU);
        } else if (funct5 == 0x1cU) {
            out.atomic_op = static_cast<uint8_t>(AMO_MAXU);
        } else {
            out.valid = false;
        }
    } else if (opcode == 0x73U) {
        if (inst == 0x00100073U) {
            out.is_ebreak = true;
        } else {
            out.valid = false;
        }
    } else {
        out.valid = false;
    }
};

{
  uint32_t inst;
  inst = decode_inst__.at<31, 0>();
  DecodedInst out;
  if (decode__vld__) {
    decode_impl__(inst, out);
    Int<177> out_packed;
    out_packed.at<0, 0>() = out.valid;
    out_packed.at<1, 1>() = out.writes_rd;
    out_packed.at<2, 2>() = out.use_rs1;
    out_packed.at<3, 3>() = out.use_rs2;
    out_packed.at<4, 4>() = out.is_load;
    out_packed.at<5, 5>() = out.is_store;
    out_packed.at<6, 6>() = out.is_branch;
    out_packed.at<7, 7>() = out.is_jal;
    out_packed.at<8, 8>() = out.is_jalr;
    out_packed.at<9, 9>() = out.is_lui;
    out_packed.at<10, 10>() = out.is_auipc;
    out_packed.at<11, 11>() = out.is_ebreak;
    out_packed.at<12, 12>() = out.is_muldiv;
    out_packed.at<13, 13>() = out.is_atomic;
    out_packed.at<14, 14>() = out.is_lr;
    out_packed.at<15, 15>() = out.is_sc;
    out_packed.at<23, 16>() = out.rd;
    out_packed.at<31, 24>() = out.rs1;
    out_packed.at<39, 32>() = out.rs2;
    out_packed.at<47, 40>() = out.alu_op;
    out_packed.at<55, 48>() = out.branch_op;
    out_packed.at<63, 56>() = out.mem_width;
    out_packed.at<64, 64>() = out.mem_unsigned;
    out_packed.at<72, 65>() = out.muldiv_op;
    out_packed.at<80, 73>() = out.atomic_op;
    out_packed.at<112, 81>() = out.inst;
    out_packed.at<176, 113>() = out.imm;
    decode_out__ = out_packed;
  }
}

}

