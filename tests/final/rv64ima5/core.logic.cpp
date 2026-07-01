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

inline IfIdStage make_ifid_invalid() {
    IfIdStage s;
    s.valid = false;
    s.pc = 0;
    s.inst = 0;
    return s;
}
inline DecodedInst make_decoded_invalid() {
    DecodedInst d;
    d.valid = false;
    d.writes_rd = false;
    d.use_rs1 = false;
    d.use_rs2 = false;
    d.is_load = false;
    d.is_store = false;
    d.is_branch = false;
    d.is_jal = false;
    d.is_jalr = false;
    d.is_lui = false;
    d.is_auipc = false;
    d.is_ebreak = false;
    d.is_muldiv = false;
    d.is_atomic = false;
    d.is_lr = false;
    d.is_sc = false;
    d.rd = 0;
    d.rs1 = 0;
    d.rs2 = 0;
    d.alu_op = static_cast<uint8_t>(ALU_ADD);
    d.branch_op = static_cast<uint8_t>(BR_NONE);
    d.mem_width = 8;
    d.mem_unsigned = false;
    d.muldiv_op = static_cast<uint8_t>(MDU_NONE);
    d.atomic_op = static_cast<uint8_t>(AMO_NONE);
    d.inst = 0;
    d.imm = 0;
    return d;
}
inline IdExStage make_idex_invalid() {
    IdExStage s;
    s.valid = false;
    s.pc = 0;
    s.rs1_val = 0;
    s.rs2_val = 0;
    s.dec = make_decoded_invalid();
    return s;
}
inline ExecResult make_exec_zero() {
    ExecResult e;
    e.valid = false;
    e.branch_taken = false;
    e.branch_target = 0;
    e.result = 0;
    e.mem_addr = 0;
    e.store_data = 0;
    return e;
}
inline ExMemStage make_exmem_invalid() {
    ExMemStage s;
    s.valid = false;
    s.pc = 0;
    s.ex = make_exec_zero();
    s.dec = make_decoded_invalid();
    return s;
}
inline MemWbStage make_memwb_invalid() {
    MemWbStage s;
    s.valid = false;
    s.pc = 0;
    s.inst = 0;
    s.writes_rd = false;
    s.rd = 0;
    s.wb_data = 0;
    s.halt = false;
    return s;
}
inline uint64_t load_extend_data(uint64_t value, uint8_t width, bool is_unsigned) {
    if (width == 1) {
        if (is_unsigned) return value & 0xffULL;
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(value & 0xffULL)));
    }
    if (width == 2) {
        if (is_unsigned) return value & 0xffffULL;
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(value & 0xffffULL)));
    }
    if (width == 4) {
        if (is_unsigned) return value & 0xffffffffULL;
        return static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value & 0xffffffffULL)));
    }
    return value;
}

struct __RegProxy_uint64_t__cycle {
const Int<64> &rdata;
bool &wen;
Int<64> &wdata;

__RegProxy_uint64_t__cycle(const Int<64> &rdata, bool &wen, Int<64> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator uint64_t() const {
  uint64_t value;
  value = rdata.at<63, 0>();
  return value;
}
uint64_t get() const {
  uint64_t value;
  value = rdata.at<63, 0>();
  return value;
}
template <uint32_t P = 0>
void setnext(const uint64_t &value) {
  static_assert(P < 1, "Port index out of range");
  Int<64> wdata_val;
  wdata_val.at<63, 0>() = value;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_uint64_t__pc {
const Int<64> &rdata;
bool &wen;
Int<64> &wdata;

__RegProxy_uint64_t__pc(const Int<64> &rdata, bool &wen, Int<64> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator uint64_t() const {
  uint64_t value;
  value = rdata.at<63, 0>();
  return value;
}
uint64_t get() const {
  uint64_t value;
  value = rdata.at<63, 0>();
  return value;
}
template <uint32_t P = 0>
void setnext(const uint64_t &value) {
  static_assert(P < 1, "Port index out of range");
  Int<64> wdata_val;
  wdata_val.at<63, 0>() = value;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_bool__halted {
const Int<1> &rdata;
bool &wen;
Int<1> &wdata;

__RegProxy_bool__halted(const Int<1> &rdata, bool &wen, Int<1> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator bool() const {
  bool value;
  value = rdata.at<0, 0>();
  return value;
}
bool get() const {
  bool value;
  value = rdata.at<0, 0>();
  return value;
}
template <uint32_t P = 0>
void setnext(const bool &value) {
  static_assert(P < 1, "Port index out of range");
  Int<1> wdata_val;
  wdata_val.at<0, 0>() = value;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_IfIdStage__if_id {
const Int<97> &rdata;
bool &wen;
Int<97> &wdata;

__RegProxy_IfIdStage__if_id(const Int<97> &rdata, bool &wen, Int<97> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator IfIdStage() const {
  IfIdStage value;
  value.valid = rdata.at<0, 0>();
  value.pc = rdata.at<64, 1>();
  value.inst = rdata.at<96, 65>();
  return value;
}
IfIdStage get() const {
  IfIdStage value;
  value.valid = rdata.at<0, 0>();
  value.pc = rdata.at<64, 1>();
  value.inst = rdata.at<96, 65>();
  return value;
}
template <uint32_t P = 0>
void setnext(const IfIdStage &value) {
  static_assert(P < 1, "Port index out of range");
  Int<97> wdata_val;
  wdata_val.at<0, 0>() = value.valid;
  wdata_val.at<64, 1>() = value.pc;
  wdata_val.at<96, 65>() = value.inst;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_IdExStage__id_ex {
const Int<370> &rdata;
bool &wen;
Int<370> &wdata;

__RegProxy_IdExStage__id_ex(const Int<370> &rdata, bool &wen, Int<370> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator IdExStage() const {
  IdExStage value;
  value.valid = rdata.at<0, 0>();
  value.pc = rdata.at<64, 1>();
  value.rs1_val = rdata.at<128, 65>();
  value.rs2_val = rdata.at<192, 129>();
  value.dec.valid = rdata.at<193, 193>();
  value.dec.writes_rd = rdata.at<194, 194>();
  value.dec.use_rs1 = rdata.at<195, 195>();
  value.dec.use_rs2 = rdata.at<196, 196>();
  value.dec.is_load = rdata.at<197, 197>();
  value.dec.is_store = rdata.at<198, 198>();
  value.dec.is_branch = rdata.at<199, 199>();
  value.dec.is_jal = rdata.at<200, 200>();
  value.dec.is_jalr = rdata.at<201, 201>();
  value.dec.is_lui = rdata.at<202, 202>();
  value.dec.is_auipc = rdata.at<203, 203>();
  value.dec.is_ebreak = rdata.at<204, 204>();
  value.dec.is_muldiv = rdata.at<205, 205>();
  value.dec.is_atomic = rdata.at<206, 206>();
  value.dec.is_lr = rdata.at<207, 207>();
  value.dec.is_sc = rdata.at<208, 208>();
  value.dec.rd = rdata.at<216, 209>();
  value.dec.rs1 = rdata.at<224, 217>();
  value.dec.rs2 = rdata.at<232, 225>();
  value.dec.alu_op = rdata.at<240, 233>();
  value.dec.branch_op = rdata.at<248, 241>();
  value.dec.mem_width = rdata.at<256, 249>();
  value.dec.mem_unsigned = rdata.at<257, 257>();
  value.dec.muldiv_op = rdata.at<265, 258>();
  value.dec.atomic_op = rdata.at<273, 266>();
  value.dec.inst = rdata.at<305, 274>();
  value.dec.imm = rdata.at<369, 306>();
  return value;
}
IdExStage get() const {
  IdExStage value;
  value.valid = rdata.at<0, 0>();
  value.pc = rdata.at<64, 1>();
  value.rs1_val = rdata.at<128, 65>();
  value.rs2_val = rdata.at<192, 129>();
  value.dec.valid = rdata.at<193, 193>();
  value.dec.writes_rd = rdata.at<194, 194>();
  value.dec.use_rs1 = rdata.at<195, 195>();
  value.dec.use_rs2 = rdata.at<196, 196>();
  value.dec.is_load = rdata.at<197, 197>();
  value.dec.is_store = rdata.at<198, 198>();
  value.dec.is_branch = rdata.at<199, 199>();
  value.dec.is_jal = rdata.at<200, 200>();
  value.dec.is_jalr = rdata.at<201, 201>();
  value.dec.is_lui = rdata.at<202, 202>();
  value.dec.is_auipc = rdata.at<203, 203>();
  value.dec.is_ebreak = rdata.at<204, 204>();
  value.dec.is_muldiv = rdata.at<205, 205>();
  value.dec.is_atomic = rdata.at<206, 206>();
  value.dec.is_lr = rdata.at<207, 207>();
  value.dec.is_sc = rdata.at<208, 208>();
  value.dec.rd = rdata.at<216, 209>();
  value.dec.rs1 = rdata.at<224, 217>();
  value.dec.rs2 = rdata.at<232, 225>();
  value.dec.alu_op = rdata.at<240, 233>();
  value.dec.branch_op = rdata.at<248, 241>();
  value.dec.mem_width = rdata.at<256, 249>();
  value.dec.mem_unsigned = rdata.at<257, 257>();
  value.dec.muldiv_op = rdata.at<265, 258>();
  value.dec.atomic_op = rdata.at<273, 266>();
  value.dec.inst = rdata.at<305, 274>();
  value.dec.imm = rdata.at<369, 306>();
  return value;
}
template <uint32_t P = 0>
void setnext(const IdExStage &value) {
  static_assert(P < 1, "Port index out of range");
  Int<370> wdata_val;
  wdata_val.at<0, 0>() = value.valid;
  wdata_val.at<64, 1>() = value.pc;
  wdata_val.at<128, 65>() = value.rs1_val;
  wdata_val.at<192, 129>() = value.rs2_val;
  wdata_val.at<193, 193>() = value.dec.valid;
  wdata_val.at<194, 194>() = value.dec.writes_rd;
  wdata_val.at<195, 195>() = value.dec.use_rs1;
  wdata_val.at<196, 196>() = value.dec.use_rs2;
  wdata_val.at<197, 197>() = value.dec.is_load;
  wdata_val.at<198, 198>() = value.dec.is_store;
  wdata_val.at<199, 199>() = value.dec.is_branch;
  wdata_val.at<200, 200>() = value.dec.is_jal;
  wdata_val.at<201, 201>() = value.dec.is_jalr;
  wdata_val.at<202, 202>() = value.dec.is_lui;
  wdata_val.at<203, 203>() = value.dec.is_auipc;
  wdata_val.at<204, 204>() = value.dec.is_ebreak;
  wdata_val.at<205, 205>() = value.dec.is_muldiv;
  wdata_val.at<206, 206>() = value.dec.is_atomic;
  wdata_val.at<207, 207>() = value.dec.is_lr;
  wdata_val.at<208, 208>() = value.dec.is_sc;
  wdata_val.at<216, 209>() = value.dec.rd;
  wdata_val.at<224, 217>() = value.dec.rs1;
  wdata_val.at<232, 225>() = value.dec.rs2;
  wdata_val.at<240, 233>() = value.dec.alu_op;
  wdata_val.at<248, 241>() = value.dec.branch_op;
  wdata_val.at<256, 249>() = value.dec.mem_width;
  wdata_val.at<257, 257>() = value.dec.mem_unsigned;
  wdata_val.at<265, 258>() = value.dec.muldiv_op;
  wdata_val.at<273, 266>() = value.dec.atomic_op;
  wdata_val.at<305, 274>() = value.dec.inst;
  wdata_val.at<369, 306>() = value.dec.imm;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_ExMemStage__ex_mem {
const Int<500> &rdata;
bool &wen;
Int<500> &wdata;

__RegProxy_ExMemStage__ex_mem(const Int<500> &rdata, bool &wen, Int<500> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator ExMemStage() const {
  ExMemStage value;
  value.valid = rdata.at<0, 0>();
  value.pc = rdata.at<64, 1>();
  value.ex.valid = rdata.at<65, 65>();
  value.ex.branch_taken = rdata.at<66, 66>();
  value.ex.branch_target = rdata.at<130, 67>();
  value.ex.result = rdata.at<194, 131>();
  value.ex.mem_addr = rdata.at<258, 195>();
  value.ex.store_data = rdata.at<322, 259>();
  value.dec.valid = rdata.at<323, 323>();
  value.dec.writes_rd = rdata.at<324, 324>();
  value.dec.use_rs1 = rdata.at<325, 325>();
  value.dec.use_rs2 = rdata.at<326, 326>();
  value.dec.is_load = rdata.at<327, 327>();
  value.dec.is_store = rdata.at<328, 328>();
  value.dec.is_branch = rdata.at<329, 329>();
  value.dec.is_jal = rdata.at<330, 330>();
  value.dec.is_jalr = rdata.at<331, 331>();
  value.dec.is_lui = rdata.at<332, 332>();
  value.dec.is_auipc = rdata.at<333, 333>();
  value.dec.is_ebreak = rdata.at<334, 334>();
  value.dec.is_muldiv = rdata.at<335, 335>();
  value.dec.is_atomic = rdata.at<336, 336>();
  value.dec.is_lr = rdata.at<337, 337>();
  value.dec.is_sc = rdata.at<338, 338>();
  value.dec.rd = rdata.at<346, 339>();
  value.dec.rs1 = rdata.at<354, 347>();
  value.dec.rs2 = rdata.at<362, 355>();
  value.dec.alu_op = rdata.at<370, 363>();
  value.dec.branch_op = rdata.at<378, 371>();
  value.dec.mem_width = rdata.at<386, 379>();
  value.dec.mem_unsigned = rdata.at<387, 387>();
  value.dec.muldiv_op = rdata.at<395, 388>();
  value.dec.atomic_op = rdata.at<403, 396>();
  value.dec.inst = rdata.at<435, 404>();
  value.dec.imm = rdata.at<499, 436>();
  return value;
}
ExMemStage get() const {
  ExMemStage value;
  value.valid = rdata.at<0, 0>();
  value.pc = rdata.at<64, 1>();
  value.ex.valid = rdata.at<65, 65>();
  value.ex.branch_taken = rdata.at<66, 66>();
  value.ex.branch_target = rdata.at<130, 67>();
  value.ex.result = rdata.at<194, 131>();
  value.ex.mem_addr = rdata.at<258, 195>();
  value.ex.store_data = rdata.at<322, 259>();
  value.dec.valid = rdata.at<323, 323>();
  value.dec.writes_rd = rdata.at<324, 324>();
  value.dec.use_rs1 = rdata.at<325, 325>();
  value.dec.use_rs2 = rdata.at<326, 326>();
  value.dec.is_load = rdata.at<327, 327>();
  value.dec.is_store = rdata.at<328, 328>();
  value.dec.is_branch = rdata.at<329, 329>();
  value.dec.is_jal = rdata.at<330, 330>();
  value.dec.is_jalr = rdata.at<331, 331>();
  value.dec.is_lui = rdata.at<332, 332>();
  value.dec.is_auipc = rdata.at<333, 333>();
  value.dec.is_ebreak = rdata.at<334, 334>();
  value.dec.is_muldiv = rdata.at<335, 335>();
  value.dec.is_atomic = rdata.at<336, 336>();
  value.dec.is_lr = rdata.at<337, 337>();
  value.dec.is_sc = rdata.at<338, 338>();
  value.dec.rd = rdata.at<346, 339>();
  value.dec.rs1 = rdata.at<354, 347>();
  value.dec.rs2 = rdata.at<362, 355>();
  value.dec.alu_op = rdata.at<370, 363>();
  value.dec.branch_op = rdata.at<378, 371>();
  value.dec.mem_width = rdata.at<386, 379>();
  value.dec.mem_unsigned = rdata.at<387, 387>();
  value.dec.muldiv_op = rdata.at<395, 388>();
  value.dec.atomic_op = rdata.at<403, 396>();
  value.dec.inst = rdata.at<435, 404>();
  value.dec.imm = rdata.at<499, 436>();
  return value;
}
template <uint32_t P = 0>
void setnext(const ExMemStage &value) {
  static_assert(P < 1, "Port index out of range");
  Int<500> wdata_val;
  wdata_val.at<0, 0>() = value.valid;
  wdata_val.at<64, 1>() = value.pc;
  wdata_val.at<65, 65>() = value.ex.valid;
  wdata_val.at<66, 66>() = value.ex.branch_taken;
  wdata_val.at<130, 67>() = value.ex.branch_target;
  wdata_val.at<194, 131>() = value.ex.result;
  wdata_val.at<258, 195>() = value.ex.mem_addr;
  wdata_val.at<322, 259>() = value.ex.store_data;
  wdata_val.at<323, 323>() = value.dec.valid;
  wdata_val.at<324, 324>() = value.dec.writes_rd;
  wdata_val.at<325, 325>() = value.dec.use_rs1;
  wdata_val.at<326, 326>() = value.dec.use_rs2;
  wdata_val.at<327, 327>() = value.dec.is_load;
  wdata_val.at<328, 328>() = value.dec.is_store;
  wdata_val.at<329, 329>() = value.dec.is_branch;
  wdata_val.at<330, 330>() = value.dec.is_jal;
  wdata_val.at<331, 331>() = value.dec.is_jalr;
  wdata_val.at<332, 332>() = value.dec.is_lui;
  wdata_val.at<333, 333>() = value.dec.is_auipc;
  wdata_val.at<334, 334>() = value.dec.is_ebreak;
  wdata_val.at<335, 335>() = value.dec.is_muldiv;
  wdata_val.at<336, 336>() = value.dec.is_atomic;
  wdata_val.at<337, 337>() = value.dec.is_lr;
  wdata_val.at<338, 338>() = value.dec.is_sc;
  wdata_val.at<346, 339>() = value.dec.rd;
  wdata_val.at<354, 347>() = value.dec.rs1;
  wdata_val.at<362, 355>() = value.dec.rs2;
  wdata_val.at<370, 363>() = value.dec.alu_op;
  wdata_val.at<378, 371>() = value.dec.branch_op;
  wdata_val.at<386, 379>() = value.dec.mem_width;
  wdata_val.at<387, 387>() = value.dec.mem_unsigned;
  wdata_val.at<395, 388>() = value.dec.muldiv_op;
  wdata_val.at<403, 396>() = value.dec.atomic_op;
  wdata_val.at<435, 404>() = value.dec.inst;
  wdata_val.at<499, 436>() = value.dec.imm;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_MemWbStage__mem_wb {
const Int<171> &rdata;
bool &wen;
Int<171> &wdata;

__RegProxy_MemWbStage__mem_wb(const Int<171> &rdata, bool &wen, Int<171> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator MemWbStage() const {
  MemWbStage value;
  value.valid = rdata.at<0, 0>();
  value.pc = rdata.at<64, 1>();
  value.inst = rdata.at<96, 65>();
  value.writes_rd = rdata.at<97, 97>();
  value.rd = rdata.at<105, 98>();
  value.wb_data = rdata.at<169, 106>();
  value.halt = rdata.at<170, 170>();
  return value;
}
MemWbStage get() const {
  MemWbStage value;
  value.valid = rdata.at<0, 0>();
  value.pc = rdata.at<64, 1>();
  value.inst = rdata.at<96, 65>();
  value.writes_rd = rdata.at<97, 97>();
  value.rd = rdata.at<105, 98>();
  value.wb_data = rdata.at<169, 106>();
  value.halt = rdata.at<170, 170>();
  return value;
}
template <uint32_t P = 0>
void setnext(const MemWbStage &value) {
  static_assert(P < 1, "Port index out of range");
  Int<171> wdata_val;
  wdata_val.at<0, 0>() = value.valid;
  wdata_val.at<64, 1>() = value.pc;
  wdata_val.at<96, 65>() = value.inst;
  wdata_val.at<97, 97>() = value.writes_rd;
  wdata_val.at<105, 98>() = value.rd;
  wdata_val.at<169, 106>() = value.wb_data;
  wdata_val.at<170, 170>() = value.halt;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_bool__fetch_waiting {
const Int<1> &rdata;
bool &wen;
Int<1> &wdata;

__RegProxy_bool__fetch_waiting(const Int<1> &rdata, bool &wen, Int<1> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator bool() const {
  bool value;
  value = rdata.at<0, 0>();
  return value;
}
bool get() const {
  bool value;
  value = rdata.at<0, 0>();
  return value;
}
template <uint32_t P = 0>
void setnext(const bool &value) {
  static_assert(P < 1, "Port index out of range");
  Int<1> wdata_val;
  wdata_val.at<0, 0>() = value;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_bool__fetch_drop_resp {
const Int<1> &rdata;
bool &wen;
Int<1> &wdata;

__RegProxy_bool__fetch_drop_resp(const Int<1> &rdata, bool &wen, Int<1> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator bool() const {
  bool value;
  value = rdata.at<0, 0>();
  return value;
}
bool get() const {
  bool value;
  value = rdata.at<0, 0>();
  return value;
}
template <uint32_t P = 0>
void setnext(const bool &value) {
  static_assert(P < 1, "Port index out of range");
  Int<1> wdata_val;
  wdata_val.at<0, 0>() = value;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_uint64_t__fetch_req_pc {
const Int<64> &rdata;
bool &wen;
Int<64> &wdata;

__RegProxy_uint64_t__fetch_req_pc(const Int<64> &rdata, bool &wen, Int<64> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator uint64_t() const {
  uint64_t value;
  value = rdata.at<63, 0>();
  return value;
}
uint64_t get() const {
  uint64_t value;
  value = rdata.at<63, 0>();
  return value;
}
template <uint32_t P = 0>
void setnext(const uint64_t &value) {
  static_assert(P < 1, "Port index out of range");
  Int<64> wdata_val;
  wdata_val.at<63, 0>() = value;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_bool__mem_waiting {
const Int<1> &rdata;
bool &wen;
Int<1> &wdata;

__RegProxy_bool__mem_waiting(const Int<1> &rdata, bool &wen, Int<1> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator bool() const {
  bool value;
  value = rdata.at<0, 0>();
  return value;
}
bool get() const {
  bool value;
  value = rdata.at<0, 0>();
  return value;
}
template <uint32_t P = 0>
void setnext(const bool &value) {
  static_assert(P < 1, "Port index out of range");
  Int<1> wdata_val;
  wdata_val.at<0, 0>() = value;
  wdata = wdata_val;
  wen = true;
}
};

struct __ReqHelper__trace_halt {
  bool & vld_ports;
  Int<64> & arg_pc;

  template <uint32_t IDX = 0>
  void call(const uint64_t & pc) {
    static_assert(IDX < 1, "Request port index out of range");
    vld_ports = true;
    arg_pc.at<63, 0>() = pc;
  }
};
struct __ReqHelper__trace_wb {
  bool & vld_ports;
  Int<32> & arg_rd;
  Int<64> & arg_data;
  Int<1> & arg_wen;

  template <uint32_t IDX = 0>
  void call(const uint32_t & rd, const uint64_t & data, const bool & wen) {
    static_assert(IDX < 1, "Request port index out of range");
    vld_ports = true;
    arg_rd.at<31, 0>() = rd;
    arg_data.at<63, 0>() = data;
    arg_wen.at<0, 0>() = wen;
  }
};
struct __ReqHelper__dcache_resp {
  bool & vld_ports;
  const Int<1> & ret_hit;
  const Int<65> & ret_resp;

  template <uint32_t IDX = 0>
  void call(bool & hit, MemResponse & resp) {
    static_assert(IDX < 1, "Request port index out of range");
    vld_ports = true;
    hit = ret_hit.at<0, 0>();
    resp.rdata = ret_resp.at<63, 0>();
    resp.success = ret_resp.at<64, 64>();
  }
};
struct __ReqHelper__dcache_req {
  bool & vld_ports;
  Int<150> & arg_req;

  template <uint32_t IDX = 0>
  void call(const MemRequest & req) {
    static_assert(IDX < 1, "Request port index out of range");
    vld_ports = true;
    arg_req.at<0, 0>() = req.valid;
    arg_req.at<1, 1>() = req.is_write;
    arg_req.at<2, 2>() = req.is_atomic;
    arg_req.at<3, 3>() = req.is_lr;
    arg_req.at<4, 4>() = req.is_sc;
    arg_req.at<5, 5>() = req.mem_unsigned;
    arg_req.at<13, 6>() = req.width;
    arg_req.at<21, 14>() = req.atomic_op;
    arg_req.at<85, 22>() = req.addr;
    arg_req.at<149, 86>() = req.wdata;
  }
};
struct __ReqHelper__icache_resp {
  bool & vld_ports;
  const Int<1> & ret_hit;
  const Int<32> & ret_inst;

  template <uint32_t IDX = 0>
  void call(bool & hit, uint32_t & inst) {
    static_assert(IDX < 1, "Request port index out of range");
    vld_ports = true;
    hit = ret_hit.at<0, 0>();
    inst = ret_inst.at<31, 0>();
  }
};
struct __ReqHelper__icache_req {
  bool & vld_ports;
  Int<64> & arg_pc;

  template <uint32_t IDX = 0>
  void call(const uint64_t & pc) {
    static_assert(IDX < 1, "Request port index out of range");
    vld_ports = true;
    arg_pc.at<63, 0>() = pc;
  }
};
struct __ChildServiceHelper__decode_inst {
  bool & vld_ports_0;
  Int<32> & arg_inst_0;
  const Int<177> & ret_out_0;

  template <uint32_t IDX = 0>
  void call(const uint32_t & inst, DecodedInst & out) {
    static_assert(IDX < 1, "Implicit request index out of range");
    if constexpr (IDX == 0) {
      vld_ports_0 = true;
      arg_inst_0.at<31, 0>() = inst;
      out.valid = ret_out_0.at<0, 0>();
      out.writes_rd = ret_out_0.at<1, 1>();
      out.use_rs1 = ret_out_0.at<2, 2>();
      out.use_rs2 = ret_out_0.at<3, 3>();
      out.is_load = ret_out_0.at<4, 4>();
      out.is_store = ret_out_0.at<5, 5>();
      out.is_branch = ret_out_0.at<6, 6>();
      out.is_jal = ret_out_0.at<7, 7>();
      out.is_jalr = ret_out_0.at<8, 8>();
      out.is_lui = ret_out_0.at<9, 9>();
      out.is_auipc = ret_out_0.at<10, 10>();
      out.is_ebreak = ret_out_0.at<11, 11>();
      out.is_muldiv = ret_out_0.at<12, 12>();
      out.is_atomic = ret_out_0.at<13, 13>();
      out.is_lr = ret_out_0.at<14, 14>();
      out.is_sc = ret_out_0.at<15, 15>();
      out.rd = ret_out_0.at<23, 16>();
      out.rs1 = ret_out_0.at<31, 24>();
      out.rs2 = ret_out_0.at<39, 32>();
      out.alu_op = ret_out_0.at<47, 40>();
      out.branch_op = ret_out_0.at<55, 48>();
      out.mem_width = ret_out_0.at<63, 56>();
      out.mem_unsigned = ret_out_0.at<64, 64>();
      out.muldiv_op = ret_out_0.at<72, 65>();
      out.atomic_op = ret_out_0.at<80, 73>();
      out.inst = ret_out_0.at<112, 81>();
      out.imm = ret_out_0.at<176, 113>();
      return;
    }
  }
};
struct __ChildServiceHelper__exec_inst {
  bool & vld_ports_0;
  Int<369> & arg_req_0;
  const Int<258> & ret_out_0;

  template <uint32_t IDX = 0>
  void call(const ExecRequest & req, ExecResult & out) {
    static_assert(IDX < 1, "Implicit request index out of range");
    if constexpr (IDX == 0) {
      vld_ports_0 = true;
      arg_req_0.at<63, 0>() = req.pc;
      arg_req_0.at<127, 64>() = req.rs1_val;
      arg_req_0.at<191, 128>() = req.rs2_val;
      arg_req_0.at<192, 192>() = req.dec.valid;
      arg_req_0.at<193, 193>() = req.dec.writes_rd;
      arg_req_0.at<194, 194>() = req.dec.use_rs1;
      arg_req_0.at<195, 195>() = req.dec.use_rs2;
      arg_req_0.at<196, 196>() = req.dec.is_load;
      arg_req_0.at<197, 197>() = req.dec.is_store;
      arg_req_0.at<198, 198>() = req.dec.is_branch;
      arg_req_0.at<199, 199>() = req.dec.is_jal;
      arg_req_0.at<200, 200>() = req.dec.is_jalr;
      arg_req_0.at<201, 201>() = req.dec.is_lui;
      arg_req_0.at<202, 202>() = req.dec.is_auipc;
      arg_req_0.at<203, 203>() = req.dec.is_ebreak;
      arg_req_0.at<204, 204>() = req.dec.is_muldiv;
      arg_req_0.at<205, 205>() = req.dec.is_atomic;
      arg_req_0.at<206, 206>() = req.dec.is_lr;
      arg_req_0.at<207, 207>() = req.dec.is_sc;
      arg_req_0.at<215, 208>() = req.dec.rd;
      arg_req_0.at<223, 216>() = req.dec.rs1;
      arg_req_0.at<231, 224>() = req.dec.rs2;
      arg_req_0.at<239, 232>() = req.dec.alu_op;
      arg_req_0.at<247, 240>() = req.dec.branch_op;
      arg_req_0.at<255, 248>() = req.dec.mem_width;
      arg_req_0.at<256, 256>() = req.dec.mem_unsigned;
      arg_req_0.at<264, 257>() = req.dec.muldiv_op;
      arg_req_0.at<272, 265>() = req.dec.atomic_op;
      arg_req_0.at<304, 273>() = req.dec.inst;
      arg_req_0.at<368, 305>() = req.dec.imm;
      out.valid = ret_out_0.at<0, 0>();
      out.branch_taken = ret_out_0.at<1, 1>();
      out.branch_target = ret_out_0.at<65, 2>();
      out.result = ret_out_0.at<129, 66>();
      out.mem_addr = ret_out_0.at<193, 130>();
      out.store_data = ret_out_0.at<257, 194>();
      return;
    }
  }
};
struct __ChildServiceHelper__rf_read2 {
  bool & vld_ports_0;
  Int<32> & arg_rs1_0;
  Int<32> & arg_rs2_0;
  const Int<128> & ret_out_0;

  template <uint32_t IDX = 0>
  void call(const uint32_t & rs1, const uint32_t & rs2, RegReadPair & out) {
    static_assert(IDX < 1, "Implicit request index out of range");
    if constexpr (IDX == 0) {
      vld_ports_0 = true;
      arg_rs1_0.at<31, 0>() = rs1;
      arg_rs2_0.at<31, 0>() = rs2;
      out.rs1_val = ret_out_0.at<63, 0>();
      out.rs2_val = ret_out_0.at<127, 64>();
      return;
    }
  }
};
struct __ChildServiceHelper__rf_write {
  bool & vld_ports_0;
  Int<32> & arg_rd_0;
  Int<64> & arg_data_0;
  Int<1> & arg_wen_0;

  template <uint32_t IDX = 0>
  void call(const uint32_t & rd, const uint64_t & data, const bool & wen) {
    static_assert(IDX < 1, "Implicit request index out of range");
    if constexpr (IDX == 0) {
      vld_ports_0 = true;
      arg_rd_0.at<31, 0>() = rd;
      arg_data_0.at<63, 0>() = data;
      arg_wen_0.at<0, 0>() = wen;
      return;
    }
  }
};
struct __ChildQueryHelper__rf_zero_and_ra {
  const Int<128> & value_0;

  RegReadPair call() const {
    RegReadPair value;
    value.rs1_val = value_0.at<63, 0>();
    value.rs2_val = value_0.at<127, 64>();
    return value;
  }
};

void LogicSubModule_Core_top_core(
  const Int<64> &rdata_cycle__,
  bool &wen_cycle__,
  Int<64> &wdata_cycle__,
  const Int<64> &rdata_pc__,
  bool &wen_pc__,
  Int<64> &wdata_pc__,
  const Int<1> &rdata_halted__,
  bool &wen_halted__,
  Int<1> &wdata_halted__,
  const Int<97> &rdata_if_id__,
  bool &wen_if_id__,
  Int<97> &wdata_if_id__,
  const Int<370> &rdata_id_ex__,
  bool &wen_id_ex__,
  Int<370> &wdata_id_ex__,
  const Int<500> &rdata_ex_mem__,
  bool &wen_ex_mem__,
  Int<500> &wdata_ex_mem__,
  const Int<171> &rdata_mem_wb__,
  bool &wen_mem_wb__,
  Int<171> &wdata_mem_wb__,
  const Int<1> &rdata_fetch_waiting__,
  bool &wen_fetch_waiting__,
  Int<1> &wdata_fetch_waiting__,
  const Int<1> &rdata_fetch_drop_resp__,
  bool &wen_fetch_drop_resp__,
  Int<1> &wdata_fetch_drop_resp__,
  const Int<64> &rdata_fetch_req_pc__,
  bool &wen_fetch_req_pc__,
  Int<64> &wdata_fetch_req_pc__,
  const Int<1> &rdata_mem_waiting__,
  bool &wen_mem_waiting__,
  Int<1> &wdata_mem_waiting__,
  bool &trace_halt__vld__,
  Int<64> &trace_halt_pc__,
  bool &trace_wb__vld__,
  Int<32> &trace_wb_rd__,
  Int<64> &trace_wb_data__,
  Int<1> &trace_wb_wen__,
  bool &dcache_resp__vld__,
  const Int<1> dcache_resp_hit__,
  const Int<65> dcache_resp_resp__,
  bool &dcache_req__vld__,
  Int<150> &dcache_req_req__,
  bool &icache_resp__vld__,
  const Int<1> icache_resp_hit__,
  const Int<32> icache_resp_inst__,
  bool &icache_req__vld__,
  Int<64> &icache_req_pc__,
  Int<128> &peek_links__query__,
  Int<134> &status__query__,
  bool & rf_write__vld__,
  Int<32> & rf_write_rd__,
  Int<64> & rf_write_data__,
  Int<1> & rf_write_wen__,
  bool & rf_read2__vld__,
  Int<32> & rf_read2_rs1__,
  Int<32> & rf_read2_rs2__,
  const Int<128> & rf_read2_out__,
  const Int<128> rf_zero_and_ra__query__,
  bool & exu_exec__vld__,
  Int<369> & exu_exec_req__,
  const Int<258> & exu_exec_out__,
  bool & dec_decode__vld__,
  Int<32> & dec_decode_inst__,
  const Int<177> & dec_decode_out__
) {
__RegProxy_uint64_t__cycle cycle(rdata_cycle__, wen_cycle__, wdata_cycle__);
__RegProxy_uint64_t__pc pc(rdata_pc__, wen_pc__, wdata_pc__);
__RegProxy_bool__halted halted(rdata_halted__, wen_halted__, wdata_halted__);
__RegProxy_IfIdStage__if_id if_id(rdata_if_id__, wen_if_id__, wdata_if_id__);
__RegProxy_IdExStage__id_ex id_ex(rdata_id_ex__, wen_id_ex__, wdata_id_ex__);
__RegProxy_ExMemStage__ex_mem ex_mem(rdata_ex_mem__, wen_ex_mem__, wdata_ex_mem__);
__RegProxy_MemWbStage__mem_wb mem_wb(rdata_mem_wb__, wen_mem_wb__, wdata_mem_wb__);
__RegProxy_bool__fetch_waiting fetch_waiting(rdata_fetch_waiting__, wen_fetch_waiting__, wdata_fetch_waiting__);
__RegProxy_bool__fetch_drop_resp fetch_drop_resp(rdata_fetch_drop_resp__, wen_fetch_drop_resp__, wdata_fetch_drop_resp__);
__RegProxy_uint64_t__fetch_req_pc fetch_req_pc(rdata_fetch_req_pc__, wen_fetch_req_pc__, wdata_fetch_req_pc__);
__RegProxy_bool__mem_waiting mem_waiting(rdata_mem_waiting__, wen_mem_waiting__, wdata_mem_waiting__);
rf_write__vld__ = false;
rf_read2__vld__ = false;
exu_exec__vld__ = false;
dec_decode__vld__ = false;

__ReqHelper__trace_halt __req_helper__trace_halt{
  trace_halt__vld__,
  trace_halt_pc__,
};
#define trace_halt __req_helper__trace_halt.call
__ReqHelper__trace_wb __req_helper__trace_wb{
  trace_wb__vld__,
  trace_wb_rd__,
  trace_wb_data__,
  trace_wb_wen__,
};
#define trace_wb __req_helper__trace_wb.call
__ReqHelper__dcache_resp __req_helper__dcache_resp{
  dcache_resp__vld__,
  dcache_resp_hit__,
  dcache_resp_resp__,
};
#define dcache_resp __req_helper__dcache_resp.call
__ReqHelper__dcache_req __req_helper__dcache_req{
  dcache_req__vld__,
  dcache_req_req__,
};
#define dcache_req __req_helper__dcache_req.call
__ReqHelper__icache_resp __req_helper__icache_resp{
  icache_resp__vld__,
  icache_resp_hit__,
  icache_resp_inst__,
};
#define icache_resp __req_helper__icache_resp.call
__ReqHelper__icache_req __req_helper__icache_req{
  icache_req__vld__,
  icache_req_pc__,
};
#define icache_req __req_helper__icache_req.call
__ChildServiceHelper__decode_inst __child_service_helper__decode_inst{
  dec_decode__vld__,
  dec_decode_inst__,
  dec_decode_out__,
};
#define decode_inst __child_service_helper__decode_inst.call
__ChildServiceHelper__exec_inst __child_service_helper__exec_inst{
  exu_exec__vld__,
  exu_exec_req__,
  exu_exec_out__,
};
#define exec_inst __child_service_helper__exec_inst.call
__ChildServiceHelper__rf_read2 __child_service_helper__rf_read2{
  rf_read2__vld__,
  rf_read2_rs1__,
  rf_read2_rs2__,
  rf_read2_out__,
};
#define rf_read2 __child_service_helper__rf_read2.call
__ChildServiceHelper__rf_write __child_service_helper__rf_write{
  rf_write__vld__,
  rf_write_rd__,
  rf_write_data__,
  rf_write_wen__,
};
#define rf_write __child_service_helper__rf_write.call
__ChildQueryHelper__rf_zero_and_ra __child_query_helper__rf_zero_and_ra{
  rf_zero_and_ra__query__,
};
#define rf_zero_and_ra __child_query_helper__rf_zero_and_ra.call

auto peek_links = [&]() -> RegReadPair {
    return rf_zero_and_ra();
};
auto status = [&]() -> CoreStatus {
    CoreStatus s;
    s.cycle = cycle;
    s.pc = pc;
    s.halted = halted;
    s.if_valid = if_id.get().valid;
    s.id_valid = id_ex.get().valid;
    s.ex_valid = ex_mem.get().valid;
    s.mem_valid = mem_wb.get().valid;
    s.wb_valid = mem_wb.get().valid;
    return s;
};
auto tick0__ = [&]() {
    uint64_t cur_pc = pc;
    bool cur_halted = halted;
    IfIdStage cur_if = if_id.get();
    IdExStage cur_id = id_ex.get();
    ExMemStage cur_ex = ex_mem.get();
    MemWbStage cur_wb = mem_wb.get();
    bool cur_fetch_waiting = fetch_waiting;
    bool cur_fetch_drop = fetch_drop_resp;
    uint64_t cur_fetch_pc = fetch_req_pc;
    bool cur_mem_waiting = mem_waiting;
    uint64_t next_pc = cur_pc;
    bool next_halted = cur_halted;
    IfIdStage next_if = cur_if;
    IdExStage next_id = cur_id;
    ExMemStage next_ex = cur_ex;
    MemWbStage next_wb = make_memwb_invalid();
    bool next_fetch_waiting = cur_fetch_waiting;
    bool next_fetch_drop = cur_fetch_drop;
    uint64_t next_fetch_pc = cur_fetch_pc;
    bool next_mem_waiting = cur_mem_waiting;
    bool wb_write_en = cur_wb.valid && cur_wb.writes_rd && (cur_wb.rd != 0U);
    if (wb_write_en) {
        rf_write(cur_wb.rd, cur_wb.wb_data, true);
    }
    trace_wb(cur_wb.rd, cur_wb.wb_data, wb_write_en);
    if (cur_wb.valid && cur_wb.halt) {
        next_halted = true;
        trace_halt(cur_wb.pc);
    }
    bool stall_frontend = false;
    bool branch_taken = false;
    uint64_t branch_target = cur_pc;
    if (cur_ex.valid) {
        if (cur_mem_waiting) {
            stall_frontend = true;
            bool dcache_hit = false;
            MemResponse mem_resp;
            mem_resp.rdata = 0;
            mem_resp.success = false;
            dcache_resp(dcache_hit, mem_resp);
            if (dcache_hit) {
                next_wb.valid = true;
                next_wb.pc = cur_ex.pc;
                next_wb.inst = cur_ex.dec.inst;
                next_wb.writes_rd = cur_ex.dec.writes_rd;
                next_wb.rd = cur_ex.dec.rd;
                next_wb.wb_data = cur_ex.ex.result;
                next_wb.halt = cur_ex.dec.is_ebreak;
                if (cur_ex.dec.is_load) {
                    next_wb.wb_data = load_extend_data(mem_resp.rdata, cur_ex.dec.mem_width, cur_ex.dec.mem_unsigned);
                } else if (cur_ex.dec.is_store) {
                    next_wb.writes_rd = false;
                } else if (cur_ex.dec.is_sc) {
                    next_wb.wb_data = mem_resp.success ? 0ULL : 1ULL;
                } else if (cur_ex.dec.is_atomic) {
                    next_wb.wb_data = load_extend_data(mem_resp.rdata, cur_ex.dec.mem_width, false);
                }
                next_ex = make_exmem_invalid();
                next_mem_waiting = false;
                stall_frontend = false;
            }
        } else if (cur_ex.dec.is_load || cur_ex.dec.is_store || cur_ex.dec.is_atomic) {
            MemRequest mem_req;
            mem_req.valid = true;
            mem_req.is_write = cur_ex.dec.is_store || cur_ex.dec.is_sc;
            mem_req.is_atomic = cur_ex.dec.is_atomic;
            mem_req.is_lr = cur_ex.dec.is_lr;
            mem_req.is_sc = cur_ex.dec.is_sc;
            mem_req.mem_unsigned = cur_ex.dec.mem_unsigned;
            mem_req.width = cur_ex.dec.mem_width;
            mem_req.atomic_op = cur_ex.dec.atomic_op;
            mem_req.addr = cur_ex.ex.mem_addr;
            mem_req.wdata = cur_ex.ex.store_data;
            dcache_req(mem_req);
            next_mem_waiting = true;
            stall_frontend = true;
        } else {
            next_wb.valid = true;
            next_wb.pc = cur_ex.pc;
            next_wb.inst = cur_ex.dec.inst;
            next_wb.writes_rd = cur_ex.dec.writes_rd;
            next_wb.rd = cur_ex.dec.rd;
            next_wb.wb_data = cur_ex.ex.result;
            next_wb.halt = cur_ex.dec.is_ebreak;
            next_ex = make_exmem_invalid();
        }
    }
    bool ex_slot_free = !next_ex.valid;
    if (!stall_frontend && cur_id.valid && ex_slot_free) {
        ExecRequest exec_req;
        exec_req.pc = cur_id.pc;
        exec_req.rs1_val = cur_id.rs1_val;
        exec_req.rs2_val = cur_id.rs2_val;
        exec_req.dec = cur_id.dec;
        ExecResult ex_res;
        exec_inst(exec_req, ex_res);
        next_ex.valid = true;
        next_ex.pc = cur_id.pc;
        next_ex.dec = cur_id.dec;
        next_ex.ex = ex_res;
        branch_taken = ex_res.branch_taken;
        branch_target = ex_res.branch_target;
        next_id = make_idex_invalid();
    }
    if (branch_taken) {
        next_if = make_ifid_invalid();
        next_id = make_idex_invalid();
        next_pc = branch_target;
    }
    bool can_decode = !stall_frontend && !branch_taken && !cur_halted && cur_if.valid && !next_id.valid;
    if (can_decode) {
        DecodedInst dec_out;
        decode_inst(cur_if.inst, dec_out);
        bool hazard = false;
        if (dec_out.valid) {
            if (dec_out.use_rs1 && dec_out.rs1 != 0U) {
                bool p0 = cur_id.valid && cur_id.dec.writes_rd && (cur_id.dec.rd == dec_out.rs1);
                bool p1 = cur_ex.valid && cur_ex.dec.writes_rd && (cur_ex.dec.rd == dec_out.rs1);
                bool p2 = cur_wb.valid && cur_wb.writes_rd && (cur_wb.rd == dec_out.rs1);
                hazard = hazard || p0 || p1 || p2;
            }
            if (dec_out.use_rs2 && dec_out.rs2 != 0U) {
                bool p0 = cur_id.valid && cur_id.dec.writes_rd && (cur_id.dec.rd == dec_out.rs2);
                bool p1 = cur_ex.valid && cur_ex.dec.writes_rd && (cur_ex.dec.rd == dec_out.rs2);
                bool p2 = cur_wb.valid && cur_wb.writes_rd && (cur_wb.rd == dec_out.rs2);
                hazard = hazard || p0 || p1 || p2;
            }
        }
        if (!hazard && dec_out.valid) {
            RegReadPair reads;
            rf_read2(dec_out.rs1, dec_out.rs2, reads);
            next_id.valid = true;
            next_id.pc = cur_if.pc;
            next_id.rs1_val = reads.rs1_val;
            next_id.rs2_val = reads.rs2_val;
            next_id.dec = dec_out;
            next_if = make_ifid_invalid();
        }
    }
    if (!cur_halted && !next_halted && !stall_frontend) {
        bool allow_fetch_resp = !next_if.valid;
        bool drop_resp = cur_fetch_drop || branch_taken;
        if (cur_fetch_waiting && allow_fetch_resp) {
            bool icache_hit = false;
            uint32_t fetched_inst = 0;
            icache_resp(icache_hit, fetched_inst);
            if (icache_hit) {
                if (!drop_resp) {
                    next_if.valid = true;
                    next_if.pc = cur_fetch_pc;
                    next_if.inst = fetched_inst;
                }
                next_fetch_waiting = false;
                next_fetch_drop = false;
            } else {
                next_fetch_waiting = true;
                next_fetch_drop = drop_resp;
            }
        } else if (branch_taken && cur_fetch_waiting) {
            next_fetch_drop = true;
        }
        if (!next_fetch_waiting) {
            icache_req(next_pc);
            next_fetch_pc = next_pc;
            next_fetch_waiting = true;
            next_fetch_drop = false;
            next_pc = next_pc + 4;
        }
    }
    cycle.setnext(cycle + 1);
    pc.setnext(next_pc);
    halted.setnext(next_halted);
    if_id.setnext(next_if);
    id_ex.setnext(next_id);
    ex_mem.setnext(next_ex);
    mem_wb.setnext(next_wb);
    fetch_waiting.setnext(next_fetch_waiting);
    fetch_drop_resp.setnext(next_fetch_drop);
    fetch_req_pc.setnext(next_fetch_pc);
    mem_waiting.setnext(next_mem_waiting);
};

{
  RegReadPair value = peek_links();
  Int<128> packed;
  packed.at<63, 0>() = value.rs1_val;
  packed.at<127, 64>() = value.rs2_val;
  peek_links__query__ = packed;
}
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
tick0__();

#undef trace_halt
#undef trace_wb
#undef dcache_resp
#undef dcache_req
#undef icache_resp
#undef icache_req
#undef decode_inst
#undef exec_inst
#undef rf_read2
#undef rf_write
#undef rf_zero_and_ra
}

