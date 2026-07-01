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

inline uint64_t sign_extend_width(uint64_t value, uint8_t width) {
    if (width == 1) {
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(value & 0xffU)));
    }
    if (width == 2) {
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(value & 0xffffU)));
    }
    if (width == 4) {
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value & 0xffffffffU)));
    }
    return value;
}
inline uint64_t do_div_signed(uint64_t lhs_u, uint64_t rhs_u) {
    int64_t lhs = static_cast<int64_t>(lhs_u);
    int64_t rhs = static_cast<int64_t>(rhs_u);
    if (rhs == 0) {
        return ~0ULL;
    }
    if (lhs == static_cast<int64_t>(0x8000000000000000ULL) && rhs == -1) {
        return lhs_u;
    }
    return static_cast<uint64_t>(lhs / rhs);
}
inline uint64_t do_rem_signed(uint64_t lhs_u, uint64_t rhs_u) {
    int64_t lhs = static_cast<int64_t>(lhs_u);
    int64_t rhs = static_cast<int64_t>(rhs_u);
    if (rhs == 0) {
        return lhs_u;
    }
    if (lhs == static_cast<int64_t>(0x8000000000000000ULL) && rhs == -1) {
        return 0;
    }
    return static_cast<uint64_t>(lhs % rhs);
}


void LogicSubModule_ExecuteUnit_top_core_exu(
  const bool exec__vld__,
  const Int<369> exec_req__,
  Int<258> &exec_out__
) {
auto exec_impl__ = [&](const ExecRequest & req, ExecResult & out) -> void {
    const DecodedInst &dec = req.dec;
    uint64_t lhs = req.rs1_val;
    uint64_t rhs = dec.use_rs2 ? req.rs2_val : dec.imm;
    uint32_t shamt = static_cast<uint32_t>(rhs & 63ULL);
    out.valid = dec.valid;
    out.branch_taken = false;
    out.branch_target = req.pc + 4;
    out.result = 0;
    out.mem_addr = lhs + dec.imm;
    out.store_data = req.rs2_val;
    if (!dec.valid) {
        return;
    }
    if (dec.is_lui) {
        out.result = dec.imm;
    } else if (dec.is_auipc) {
        out.result = req.pc + dec.imm;
    } else if (dec.is_jal) {
        out.result = req.pc + 4;
        out.branch_taken = true;
        out.branch_target = req.pc + dec.imm;
    } else if (dec.is_jalr) {
        out.result = req.pc + 4;
        out.branch_taken = true;
        out.branch_target = (lhs + dec.imm) & ~1ULL;
    } else if (dec.is_branch) {
        bool take = false;
        if (dec.branch_op == BR_EQ) take = (lhs == req.rs2_val);
        else if (dec.branch_op == BR_NE) take = (lhs != req.rs2_val);
        else if (dec.branch_op == BR_LT) take = (static_cast<int64_t>(lhs) < static_cast<int64_t>(req.rs2_val));
        else if (dec.branch_op == BR_GE) take = (static_cast<int64_t>(lhs) >= static_cast<int64_t>(req.rs2_val));
        else if (dec.branch_op == BR_LTU) take = (lhs < req.rs2_val);
        else if (dec.branch_op == BR_GEU) take = (lhs >= req.rs2_val);
        out.branch_taken = take;
        out.branch_target = req.pc + dec.imm;
    } else if (dec.is_muldiv) {
        if (dec.muldiv_op == MDU_MUL) {
            out.result = static_cast<uint64_t>(static_cast<__int128>(lhs) * static_cast<__int128>(req.rs2_val));
        } else if (dec.muldiv_op == MDU_MULH) {
            __int128 prod = static_cast<__int128>(static_cast<int64_t>(lhs)) *
                            static_cast<__int128>(static_cast<int64_t>(req.rs2_val));
            out.result = static_cast<uint64_t>(static_cast<unsigned __int128>(prod) >> 64);
        } else if (dec.muldiv_op == MDU_MULHSU) {
            __int128 prod = static_cast<__int128>(static_cast<int64_t>(lhs)) *
                            static_cast<unsigned __int128>(req.rs2_val);
            out.result = static_cast<uint64_t>(static_cast<unsigned __int128>(prod) >> 64);
        } else if (dec.muldiv_op == MDU_MULHU) {
            unsigned __int128 prod = static_cast<unsigned __int128>(lhs) *
                                     static_cast<unsigned __int128>(req.rs2_val);
            out.result = static_cast<uint64_t>(prod >> 64);
        } else if (dec.muldiv_op == MDU_DIV) {
            out.result = do_div_signed(lhs, req.rs2_val);
        } else if (dec.muldiv_op == MDU_DIVU) {
            out.result = (req.rs2_val == 0) ? ~0ULL : (lhs / req.rs2_val);
        } else if (dec.muldiv_op == MDU_REM) {
            out.result = do_rem_signed(lhs, req.rs2_val);
        } else if (dec.muldiv_op == MDU_REMU) {
            out.result = (req.rs2_val == 0) ? lhs : (lhs % req.rs2_val);
        }
    } else if (dec.is_load || dec.is_store || dec.is_atomic) {
        out.mem_addr = lhs + dec.imm;
        out.store_data = req.rs2_val;
    } else if (dec.is_ebreak) {
        out.result = 0;
    } else if (dec.alu_op == ALU_ADD) {
        out.result = lhs + rhs;
    } else if (dec.alu_op == ALU_SUB) {
        out.result = lhs - rhs;
    } else if (dec.alu_op == ALU_SLL) {
        out.result = lhs << shamt;
    } else if (dec.alu_op == ALU_SLT) {
        out.result = (static_cast<int64_t>(lhs) < static_cast<int64_t>(rhs)) ? 1ULL : 0ULL;
    } else if (dec.alu_op == ALU_SLTU) {
        out.result = (lhs < rhs) ? 1ULL : 0ULL;
    } else if (dec.alu_op == ALU_XOR) {
        out.result = lhs ^ rhs;
    } else if (dec.alu_op == ALU_SRL) {
        out.result = lhs >> shamt;
    } else if (dec.alu_op == ALU_SRA) {
        out.result = static_cast<uint64_t>(static_cast<int64_t>(lhs) >> shamt);
    } else if (dec.alu_op == ALU_OR) {
        out.result = lhs | rhs;
    } else if (dec.alu_op == ALU_AND) {
        out.result = lhs & rhs;
    } else if (dec.alu_op == ALU_COPY_RS1) {
        out.result = lhs;
    } else if (dec.alu_op == ALU_COPY_RS2) {
        out.result = rhs;
    }
};

{
  ExecRequest req;
  req.pc = exec_req__.at<63, 0>();
  req.rs1_val = exec_req__.at<127, 64>();
  req.rs2_val = exec_req__.at<191, 128>();
  req.dec.valid = exec_req__.at<192, 192>();
  req.dec.writes_rd = exec_req__.at<193, 193>();
  req.dec.use_rs1 = exec_req__.at<194, 194>();
  req.dec.use_rs2 = exec_req__.at<195, 195>();
  req.dec.is_load = exec_req__.at<196, 196>();
  req.dec.is_store = exec_req__.at<197, 197>();
  req.dec.is_branch = exec_req__.at<198, 198>();
  req.dec.is_jal = exec_req__.at<199, 199>();
  req.dec.is_jalr = exec_req__.at<200, 200>();
  req.dec.is_lui = exec_req__.at<201, 201>();
  req.dec.is_auipc = exec_req__.at<202, 202>();
  req.dec.is_ebreak = exec_req__.at<203, 203>();
  req.dec.is_muldiv = exec_req__.at<204, 204>();
  req.dec.is_atomic = exec_req__.at<205, 205>();
  req.dec.is_lr = exec_req__.at<206, 206>();
  req.dec.is_sc = exec_req__.at<207, 207>();
  req.dec.rd = exec_req__.at<215, 208>();
  req.dec.rs1 = exec_req__.at<223, 216>();
  req.dec.rs2 = exec_req__.at<231, 224>();
  req.dec.alu_op = exec_req__.at<239, 232>();
  req.dec.branch_op = exec_req__.at<247, 240>();
  req.dec.mem_width = exec_req__.at<255, 248>();
  req.dec.mem_unsigned = exec_req__.at<256, 256>();
  req.dec.muldiv_op = exec_req__.at<264, 257>();
  req.dec.atomic_op = exec_req__.at<272, 265>();
  req.dec.inst = exec_req__.at<304, 273>();
  req.dec.imm = exec_req__.at<368, 305>();
  ExecResult out;
  if (exec__vld__) {
    exec_impl__(req, out);
    Int<258> out_packed;
    out_packed.at<0, 0>() = out.valid;
    out_packed.at<1, 1>() = out.branch_taken;
    out_packed.at<65, 2>() = out.branch_target;
    out_packed.at<129, 66>() = out.result;
    out_packed.at<193, 130>() = out.mem_addr;
    out_packed.at<257, 194>() = out.store_data;
    exec_out__ = out_packed;
  }
}

}

