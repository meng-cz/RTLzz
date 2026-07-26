#include <array>
#include <cstdint>
#include <fixint.hpp>
#include <type_traits>

using std::array;

using int8 = int8_t;
using uint8 = uint8_t;
using int16 = int16_t;
using uint16 = uint16_t;
using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;

struct ProxyMeta {
    uint8_t tag;
    bool valid;
};

struct ProxyPayload {
    uint32_t data;
    ProxyMeta meta;
};

using PayloadPair = std::array<ProxyPayload, 2>;


#pragma input_port rdata_addr_reg__
Int<32> rdata_addr_reg__;
#pragma output_port wen_addr_reg__
bool wen_addr_reg__;
#pragma output_port wdata_addr_reg__
Int<32> wdata_addr_reg__;
#pragma output_port holdnext_addr_reg__
bool holdnext_addr_reg__;
#pragma output_port resetnext_addr_reg__
bool resetnext_addr_reg__;
#pragma output_port resetvalue_addr_reg__
Int<32> resetvalue_addr_reg__;
#pragma output_port emit_payload__vld__
bool emit_payload__vld__;
#pragma input_port emit_payload__rdy__
bool emit_payload__rdy__;
#pragma output_port emit_payload_payload__
Int<41> emit_payload_payload__;
#pragma input_port emit_payload_echoed__
Int<41> emit_payload_echoed__;
#pragma output_port done__vld__
bool done__vld__;
#pragma output_port done_code__
Int<32> done_code__;
#pragma input_port simple_q__enqready__
bool simple_q__enqready__;
#pragma input_port simple_q__deqvalid__
bool simple_q__deqvalid__;
#pragma output_port simple_q__enqvalid__
bool simple_q__enqvalid__;
#pragma output_port simple_q__deqready__
bool simple_q__deqready__;
#pragma output_port simple_q__enqdata__
Int<41> simple_q__enqdata__;
#pragma input_port simple_q__deqdata__
Int<41> simple_q__deqdata__;
#pragma output_port simple_q__clrnext__
bool simple_q__clrnext__;
#pragma input_port mp_q__enqready__
uint32_t mp_q__enqready__;
#pragma input_port mp_q__deqvalid__
uint32_t mp_q__deqvalid__;
#pragma output_port mp_q__enqvalid__
Int<2> mp_q__enqvalid__;
#pragma output_port mp_q__deqready__
Int<2> mp_q__deqready__;
#pragma output_port mp_q__enqdata__
std::array<Int<41>, 2> mp_q__enqdata__;
#pragma input_port mp_q__deqdata__
std::array<Int<41>, 2> mp_q__deqdata__;
#pragma output_port mp_q__clrnext__
bool mp_q__clrnext__;
#pragma input_port bram_rw_mem__s2_rdata
Int<41> bram_rw_mem__s2_rdata;
#pragma output_port bram_rw_mem__s1_en
bool bram_rw_mem__s1_en;
#pragma output_port bram_rw_mem__s1_we
bool bram_rw_mem__s1_we;
#pragma output_port bram_rw_mem__s1_addr
Int<3> bram_rw_mem__s1_addr;
#pragma output_port bram_rw_mem__s1_wdata
Int<41> bram_rw_mem__s1_wdata;
#pragma output_port bram_bank_mem__s1_readreq
std::array<bool, 2> bram_bank_mem__s1_readreq;
#pragma output_port bram_bank_mem__s1_readaddr
std::array<Int<3>, 2> bram_bank_mem__s1_readaddr;
#pragma input_port bram_bank_mem__s2_readdata
std::array<Int<41>, 2> bram_bank_mem__s2_readdata;
#pragma output_port bram_bank_mem__s1_write
std::array<bool, 2> bram_bank_mem__s1_write;
#pragma output_port bram_bank_mem__s1_writeaddr
std::array<Int<3>, 2> bram_bank_mem__s1_writeaddr;
#pragma output_port bram_bank_mem__s1_writedata
std::array<Int<41>, 2> bram_bank_mem__s1_writedata;
#pragma output_port rom_coeff_rom__s1_readreq
std::array<bool, 2> rom_coeff_rom__s1_readreq;
#pragma output_port rom_coeff_rom__s1_readaddr
std::array<Int<3>, 2> rom_coeff_rom__s1_readaddr;
#pragma input_port rom_coeff_rom__s2_readdata
std::array<Int<16>, 2> rom_coeff_rom__s2_readdata;

uint32_t __vul_read_reg_addr_reg() {
  const Int<32> &__vul_rdata = rdata_addr_reg__;
  uint32_t value = 0;
  value = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_addr_reg(uint32_t value) {
  Int<32> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value);
  wdata_addr_reg__ = __vul_reg_wdata;
  wen_addr_reg__ = true;
}
void __vul_reg_holdnext_addr_reg() {
  holdnext_addr_reg__ = true;
}
void __vul_reg_resetnext_addr_reg() {
  resetnext_addr_reg__ = true;
}

ProxyPayload __vul_queue_unpack_mp_q(const Int<41> &__vul_queue_packed) {
  ProxyPayload value = {};
  value.data = Int<32>(__vul_queue_packed.at<31, 0>()).template to<typename std::remove_reference<decltype(value.data)>::type>();
  value.meta.tag = Int<8>(__vul_queue_packed.at<39, 32>()).template to<typename std::remove_reference<decltype(value.meta.tag)>::type>();
  value.meta.valid = ReduceOr(__vul_queue_packed.at<40, 40>());
  return value;
}

std::array<ProxyPayload, 2> __vul_queue_front_mp_q() {
  std::array<ProxyPayload, 2> __vul_queue_values = {};
  if (mp_q__deqvalid__ > 0) {
    __vul_queue_values[0] = __vul_queue_unpack_mp_q(mp_q__deqdata__[0]);
  }
  if (mp_q__deqvalid__ > 1) {
    __vul_queue_values[1] = __vul_queue_unpack_mp_q(mp_q__deqdata__[1]);
  }
  return __vul_queue_values;
}
uint32_t __vul_queue_enqready_mp_q() {
  return mp_q__enqready__;
}
uint32_t __vul_queue_deqvalid_mp_q() {
  return mp_q__deqvalid__;
}
void __vul_queue_enqnext_mp_q(std::array<ProxyPayload, 2> values, uint32_t num = 2) {
  std::array<ProxyPayload, 2> __vul_queue_values = (values);
  uint32_t __vul_queue_req = (num) < 2 ? (num) : 2;
  if (__vul_queue_req > 0) {
    ProxyPayload __vul_queue_value = __vul_queue_values[0];
    Int<41> __vul_queue_packed = 0;
    __vul_queue_packed.at<31, 0>() = Int<32>(__vul_queue_value.data);
    __vul_queue_packed.at<39, 32>() = Int<8>(__vul_queue_value.meta.tag);
    __vul_queue_packed.at<40, 40>() = Int<1>(__vul_queue_value.meta.valid);
    mp_q__enqdata__[0] = __vul_queue_packed;
  }
  if (__vul_queue_req > 1) {
    ProxyPayload __vul_queue_value = __vul_queue_values[1];
    Int<41> __vul_queue_packed = 0;
    __vul_queue_packed.at<31, 0>() = Int<32>(__vul_queue_value.data);
    __vul_queue_packed.at<39, 32>() = Int<8>(__vul_queue_value.meta.tag);
    __vul_queue_packed.at<40, 40>() = Int<1>(__vul_queue_value.meta.valid);
    mp_q__enqdata__[1] = __vul_queue_packed;
  }
  mp_q__enqvalid__ = __vul_queue_req;
}
void __vul_queue_deqnext_mp_q(uint32_t num = 2) {
  uint32_t __vul_queue_req = (num) < 2 ? (num) : 2;
  mp_q__deqready__ = __vul_queue_req;
}
void __vul_queue_clrnext_mp_q() {
  mp_q__clrnext__ = true;
}

ProxyPayload __vul_queue_unpack_simple_q(const Int<41> &__vul_queue_packed) {
  ProxyPayload value = {};
  value.data = Int<32>(__vul_queue_packed.at<31, 0>()).template to<typename std::remove_reference<decltype(value.data)>::type>();
  value.meta.tag = Int<8>(__vul_queue_packed.at<39, 32>()).template to<typename std::remove_reference<decltype(value.meta.tag)>::type>();
  value.meta.valid = ReduceOr(__vul_queue_packed.at<40, 40>());
  return value;
}

ProxyPayload __vul_queue_front_simple_q() {
  return __vul_queue_unpack_simple_q(simple_q__deqdata__);
}
bool __vul_queue_enqready_simple_q() {
  return simple_q__enqready__;
}
bool __vul_queue_deqvalid_simple_q() {
  return simple_q__deqvalid__;
}
void __vul_queue_enqnext_simple_q(ProxyPayload value) {
  ProxyPayload __vul_queue_value = (value);
  Int<41> __vul_queue_packed = 0;
  __vul_queue_packed.at<31, 0>() = Int<32>(__vul_queue_value.data);
  __vul_queue_packed.at<39, 32>() = Int<8>(__vul_queue_value.meta.tag);
  __vul_queue_packed.at<40, 40>() = Int<1>(__vul_queue_value.meta.valid);
  simple_q__enqdata__ = __vul_queue_packed;
  simple_q__enqvalid__ = true;
}
void __vul_queue_deqnext_simple_q() {
  simple_q__deqready__ = true;
}
void __vul_queue_clrnext_simple_q() {
  simple_q__clrnext__ = true;
}

template <uint32_t P = 0>
Int<16> __vul_rom_readdata_coeff_rom() {
  return rom_coeff_rom__s2_readdata[P];
}
template <uint32_t P = 0>
void __vul_rom_readreq_coeff_rom(Int<3> addr) {
  rom_coeff_rom__s1_readreq[P] = true;
  rom_coeff_rom__s1_readaddr[P] = addr;
}

ProxyPayload __vul_bram_unpack_bank_mem(const Int<41> &__vul_bram_packed) {
  ProxyPayload value = {};
  value.data = Int<32>(__vul_bram_packed.at<31, 0>()).template to<typename std::remove_reference<decltype(value.data)>::type>();
  value.meta.tag = Int<8>(__vul_bram_packed.at<39, 32>()).template to<typename std::remove_reference<decltype(value.meta.tag)>::type>();
  value.meta.valid = ReduceOr(__vul_bram_packed.at<40, 40>());
  return value;
}

template <uint32_t P = 0>
void __vul_bram_readreq_bank_mem(Int<3> addr) {
  bram_bank_mem__s1_readreq[P] = true;
  bram_bank_mem__s1_readaddr[P] = addr;
}
template <uint32_t P = 0>
ProxyPayload __vul_bram_readdata_bank_mem() {
  return __vul_bram_unpack_bank_mem(bram_bank_mem__s2_readdata[P]);
}
template <uint32_t P = 0>
void __vul_bram_write_bank_mem(Int<3> addr, ProxyPayload value) {
  ProxyPayload __vul_bram_wdata_value = (value);
  Int<41> __vul_bram_packed = 0;
  __vul_bram_packed.at<31, 0>() = Int<32>(__vul_bram_wdata_value.data);
  __vul_bram_packed.at<39, 32>() = Int<8>(__vul_bram_wdata_value.meta.tag);
  __vul_bram_packed.at<40, 40>() = Int<1>(__vul_bram_wdata_value.meta.valid);
  bram_bank_mem__s1_write[P] = true;
  bram_bank_mem__s1_writeaddr[P] = (addr);
  bram_bank_mem__s1_writedata[P] = __vul_bram_packed;
}

ProxyPayload __vul_bram_unpack_rw_mem(const Int<41> &__vul_bram_packed) {
  ProxyPayload value = {};
  value.data = Int<32>(__vul_bram_packed.at<31, 0>()).template to<typename std::remove_reference<decltype(value.data)>::type>();
  value.meta.tag = Int<8>(__vul_bram_packed.at<39, 32>()).template to<typename std::remove_reference<decltype(value.meta.tag)>::type>();
  value.meta.valid = ReduceOr(__vul_bram_packed.at<40, 40>());
  return value;
}

void __vul_bram_req_rw_mem(Int<3> addr, ProxyPayload value, bool write) {
  ProxyPayload __vul_bram_wdata_value = (value);
  Int<41> __vul_bram_packed = 0;
  __vul_bram_packed.at<31, 0>() = Int<32>(__vul_bram_wdata_value.data);
  __vul_bram_packed.at<39, 32>() = Int<8>(__vul_bram_wdata_value.meta.tag);
  __vul_bram_packed.at<40, 40>() = Int<1>(__vul_bram_wdata_value.meta.valid);
  bram_rw_mem__s1_en = true;
  bram_rw_mem__s1_we = (write);
  bram_rw_mem__s1_addr = (addr);
  bram_rw_mem__s1_wdata = __vul_bram_packed;
}
ProxyPayload __vul_bram_readdata_rw_mem() {
  return __vul_bram_unpack_rw_mem(bram_rw_mem__s2_rdata);
}

void __vul_req_call_done(uint32_t code) {
  done__vld__ = true;
  uint32_t __vul_req_arg_code = (code);
  done_code__.at<31, 0>() = Int<32>(__vul_req_arg_code);
}

bool __vul_req_call_emit_payload(ProxyPayload payload, ProxyPayload &echoed) {
  emit_payload__vld__ = true;
  ProxyPayload __vul_req_arg_payload = (payload);
  emit_payload_payload__.at<31, 0>() = Int<32>(__vul_req_arg_payload.data);
  emit_payload_payload__.at<39, 32>() = Int<8>(__vul_req_arg_payload.meta.tag);
  emit_payload_payload__.at<40, 40>() = Int<1>(__vul_req_arg_payload.meta.valid);
  echoed.data = emit_payload_echoed__.at<31, 0>();
  echoed.meta.tag = emit_payload_echoed__.at<39, 32>();
  echoed.meta.valid = emit_payload_echoed__.at<40, 40>();
  return emit_payload__rdy__;
}

void LogicSubModule_Top_top() {
constexpr int64_t PROXY_MEM_SIZE = 8;

holdnext_addr_reg__ = false;
resetnext_addr_reg__ = false;
wen_addr_reg__ = false;
wdata_addr_reg__ = 0;
emit_payload__vld__ = false;
emit_payload_payload__ = 0;
done__vld__ = false;
done_code__ = 0;
simple_q__enqvalid__ = false;
simple_q__deqready__ = false;
simple_q__enqdata__ = 0;
simple_q__clrnext__ = false;
mp_q__enqvalid__ = 0;
mp_q__deqready__ = 0;
mp_q__enqdata__[0] = 0;
mp_q__enqdata__[1] = 0;
mp_q__clrnext__ = false;
bram_rw_mem__s1_en = false;
bram_rw_mem__s1_we = false;
bram_rw_mem__s1_addr = 0;
bram_rw_mem__s1_wdata = 0;
bram_bank_mem__s1_readreq[0] = false;
bram_bank_mem__s1_readaddr[0] = 0;
bram_bank_mem__s1_readreq[1] = false;
bram_bank_mem__s1_readaddr[1] = 0;
bram_bank_mem__s1_write[0] = false;
bram_bank_mem__s1_writeaddr[0] = 0;
bram_bank_mem__s1_writedata[0] = 0;
bram_bank_mem__s1_write[1] = false;
bram_bank_mem__s1_writeaddr[1] = 0;
bram_bank_mem__s1_writedata[1] = 0;
rom_coeff_rom__s1_readreq[0] = false;
rom_coeff_rom__s1_readaddr[0] = 0;
rom_coeff_rom__s1_readreq[1] = false;
rom_coeff_rom__s1_readaddr[1] = 0;

{
  uint32_t addr_reg;
    addr_reg = 0;
  Int<32> addr_reg_flatten = 0;
  addr_reg_flatten.at<31, 0>() = Int<32>(addr_reg);
  resetvalue_addr_reg__ = addr_reg_flatten;
}

auto tick0__ = [&]() {
    uint32_t addr = __vul_read_reg_addr_reg() & (PROXY_MEM_SIZE - 1);
    ProxyPayload payload;
    payload.data = __vul_read_reg_addr_reg() + __vul_rom_readdata_coeff_rom<0>().template to<uint32_t>();
    payload.meta.tag = static_cast<uint8_t>(addr);
    payload.meta.valid = true;
    __vul_rom_readreq_coeff_rom<0>(addr);
    __vul_rom_readreq_coeff_rom<1>((addr + 1) & (PROXY_MEM_SIZE - 1));
    if (__vul_queue_enqready_simple_q()) {
        __vul_queue_enqnext_simple_q(payload);
    }
    if (__vul_queue_deqvalid_simple_q()) {
        ProxyPayload head = __vul_queue_front_simple_q();
        __vul_bram_write_bank_mem<0>(addr, head);
        __vul_queue_deqnext_simple_q();
    }
    if (addr == 0) {
        __vul_queue_clrnext_simple_q();
    }
    PayloadPair pair;
    pair[0] = payload;
    pair[1] = payload;
    pair[1].data = __vul_bram_readdata_bank_mem<1>().data + __vul_rom_readdata_coeff_rom<1>().template to<uint32_t>();
    if (__vul_queue_enqready_mp_q() >= 2) {
        __vul_queue_enqnext_mp_q(pair, 2);
    }
    if (__vul_queue_deqvalid_mp_q() > 0) {
        PayloadPair popped = __vul_queue_front_mp_q();
        __vul_bram_req_rw_mem(addr, popped[0], true);
        __vul_queue_deqnext_mp_q(1);
    }
    if (addr == 3) {
        __vul_queue_clrnext_mp_q();
    }
    __vul_bram_readreq_bank_mem<0>(addr);
    __vul_bram_readreq_bank_mem<1>((addr + 1) & (PROXY_MEM_SIZE - 1));
    ProxyPayload echoed;
    if (__vul_req_call_emit_payload(payload, echoed)) {
        __vul_bram_req_rw_mem((addr + 1) & (PROXY_MEM_SIZE - 1), echoed, true);
    }
    ProxyPayload rw_read = __vul_bram_readdata_rw_mem();
    __vul_req_call_done(rw_read.data + __vul_rom_readdata_coeff_rom<0>().template to<uint32_t>());
    __vul_reg_setnext_addr_reg<0>(__vul_read_reg_addr_reg() + 1);
};

tick0__();

}
