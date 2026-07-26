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

struct ReadStageReg {
    Int<32> addr;
    bool valid;
};

struct TagEntry {
    Int<22> tag;
    bool valid;
};


#pragma input_port rdata_read_stage__
Int<33> rdata_read_stage__;
#pragma output_port wen_read_stage__
bool wen_read_stage__;
#pragma output_port wdata_read_stage__
Int<33> wdata_read_stage__;
#pragma output_port holdnext_read_stage__
bool holdnext_read_stage__;
#pragma output_port resetnext_read_stage__
bool resetnext_read_stage__;
#pragma output_port resetvalue_read_stage__
Int<33> resetvalue_read_stage__;
#pragma output_port readresp_s1__vld__
bool readresp_s1__vld__;
#pragma output_port readresp_s1_hit__
Int<1> readresp_s1_hit__;
#pragma output_port readresp_s1_data__
Int<64> readresp_s1_data__;
#pragma input_port refill_s0__vld__
bool refill_s0__vld__;
#pragma input_port refill_s0_addr__
Int<32> refill_s0_addr__;
#pragma input_port refill_s0_data__
Int<64> refill_s0_data__;
#pragma input_port read_s0__vld__
bool read_s0__vld__;
#pragma input_port read_s0_addr__
Int<32> read_s0_addr__;
#pragma output_port bram_tag_array__s1_readreq
std::array<bool, 1> bram_tag_array__s1_readreq;
#pragma output_port bram_tag_array__s1_readaddr
std::array<Int<10>, 1> bram_tag_array__s1_readaddr;
#pragma input_port bram_tag_array__s2_readdata
std::array<Int<23>, 1> bram_tag_array__s2_readdata;
#pragma output_port bram_tag_array__s1_write
std::array<bool, 1> bram_tag_array__s1_write;
#pragma output_port bram_tag_array__s1_writeaddr
std::array<Int<10>, 1> bram_tag_array__s1_writeaddr;
#pragma output_port bram_tag_array__s1_writedata
std::array<Int<23>, 1> bram_tag_array__s1_writedata;
#pragma output_port bram_data_array__s1_readreq
std::array<bool, 1> bram_data_array__s1_readreq;
#pragma output_port bram_data_array__s1_readaddr
std::array<Int<10>, 1> bram_data_array__s1_readaddr;
#pragma input_port bram_data_array__s2_readdata
std::array<Int<64>, 1> bram_data_array__s2_readdata;
#pragma output_port bram_data_array__s1_write
std::array<bool, 1> bram_data_array__s1_write;
#pragma output_port bram_data_array__s1_writeaddr
std::array<Int<10>, 1> bram_data_array__s1_writeaddr;
#pragma output_port bram_data_array__s1_writedata
std::array<Int<64>, 1> bram_data_array__s1_writedata;

ReadStageReg __vul_read_reg_read_stage() {
  const Int<33> &__vul_rdata = rdata_read_stage__;
  ReadStageReg value = {};
  value.addr = Int<32>(__vul_rdata.at<31, 0>());
  value.valid = ReduceOr(__vul_rdata.at<32, 32>());
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_read_stage(ReadStageReg value) {
  Int<33> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value.addr);
  __vul_reg_wdata.at<32, 32>() = Int<1>(value.valid);
  wdata_read_stage__ = __vul_reg_wdata;
  wen_read_stage__ = true;
}
void __vul_reg_holdnext_read_stage() {
  holdnext_read_stage__ = true;
}
void __vul_reg_resetnext_read_stage() {
  resetnext_read_stage__ = true;
}

Int<64> __vul_bram_unpack_data_array(const Int<64> &__vul_bram_packed) {
  Int<64> value = {};
  value = Int<64>(__vul_bram_packed.at<63, 0>());
  return value;
}

template <uint32_t P = 0>
void __vul_bram_readreq_data_array(Int<10> addr) {
  bram_data_array__s1_readreq[P] = true;
  bram_data_array__s1_readaddr[P] = addr;
}
template <uint32_t P = 0>
Int<64> __vul_bram_readdata_data_array() {
  return __vul_bram_unpack_data_array(bram_data_array__s2_readdata[P]);
}
template <uint32_t P = 0>
void __vul_bram_write_data_array(Int<10> addr, Int<64> value) {
  Int<64> __vul_bram_wdata_value = (value);
  Int<64> __vul_bram_packed = 0;
  __vul_bram_packed.at<63, 0>() = Int<64>(__vul_bram_wdata_value);
  bram_data_array__s1_write[P] = true;
  bram_data_array__s1_writeaddr[P] = (addr);
  bram_data_array__s1_writedata[P] = __vul_bram_packed;
}

TagEntry __vul_bram_unpack_tag_array(const Int<23> &__vul_bram_packed) {
  TagEntry value = {};
  value.tag = Int<22>(__vul_bram_packed.at<21, 0>());
  value.valid = ReduceOr(__vul_bram_packed.at<22, 22>());
  return value;
}

template <uint32_t P = 0>
void __vul_bram_readreq_tag_array(Int<10> addr) {
  bram_tag_array__s1_readreq[P] = true;
  bram_tag_array__s1_readaddr[P] = addr;
}
template <uint32_t P = 0>
TagEntry __vul_bram_readdata_tag_array() {
  return __vul_bram_unpack_tag_array(bram_tag_array__s2_readdata[P]);
}
template <uint32_t P = 0>
void __vul_bram_write_tag_array(Int<10> addr, TagEntry value) {
  TagEntry __vul_bram_wdata_value = (value);
  Int<23> __vul_bram_packed = 0;
  __vul_bram_packed.at<21, 0>() = Int<22>(__vul_bram_wdata_value.tag);
  __vul_bram_packed.at<22, 22>() = Int<1>(__vul_bram_wdata_value.valid);
  bram_tag_array__s1_write[P] = true;
  bram_tag_array__s1_writeaddr[P] = (addr);
  bram_tag_array__s1_writedata[P] = __vul_bram_packed;
}

void __vul_req_call_readresp_s1(bool hit, Int<64> data) {
  readresp_s1__vld__ = true;
  bool __vul_req_arg_hit = (hit);
  readresp_s1_hit__.at<0, 0>() = Int<1>(__vul_req_arg_hit);
  Int<64> __vul_req_arg_data = (data);
  readresp_s1_data__.at<63, 0>() = Int<64>(__vul_req_arg_data);
}

void LogicSubModule_SimpleCache_top() {
constexpr int64_t ADDR_WIDTH = 32;
constexpr int64_t DATA_WIDTH = 64;
constexpr int64_t INDEX_SIZE = 1024;
constexpr int64_t INDEX_WIDTH = 10;
constexpr int64_t TAG_WIDTH = 22;

bool read_inputed;
{
    read_inputed = false;
}
holdnext_read_stage__ = false;
resetnext_read_stage__ = false;
wen_read_stage__ = false;
wdata_read_stage__ = 0;
readresp_s1__vld__ = false;
readresp_s1_hit__ = 0;
readresp_s1_data__ = 0;
bram_tag_array__s1_readreq[0] = false;
bram_tag_array__s1_readaddr[0] = 0;
bram_tag_array__s1_write[0] = false;
bram_tag_array__s1_writeaddr[0] = 0;
bram_tag_array__s1_writedata[0] = 0;
bram_data_array__s1_readreq[0] = false;
bram_data_array__s1_readaddr[0] = 0;
bram_data_array__s1_write[0] = false;
bram_data_array__s1_writeaddr[0] = 0;
bram_data_array__s1_writedata[0] = 0;

{
  ReadStageReg read_stage;
    read_stage.addr = 0;
    read_stage.valid = false;
  Int<33> read_stage_flatten = 0;
  read_stage_flatten.at<31, 0>() = Int<32>(read_stage.addr);
  read_stage_flatten.at<32, 32>() = Int<1>(read_stage.valid);
  resetvalue_read_stage__ = read_stage_flatten;
}

auto refill_s0_impl__ = [&](Int<32> addr, Int<64> data) -> void {
    Int<INDEX_WIDTH> index = addr.at<INDEX_WIDTH - 1, 0>();
    Int<TAG_WIDTH> tag = addr.at<ADDR_WIDTH - 1, INDEX_WIDTH>();
    TagEntry tag_entry;
    tag_entry.tag = tag;
    tag_entry.valid = true;
    __vul_bram_write_tag_array<0>(index, tag_entry);
    __vul_bram_write_data_array<0>(index, data);
};
auto read_s0_impl__ = [&](Int<32> addr) -> void {
    ReadStageReg s0;
    s0.addr = addr;
    s0.valid = true;
    __vul_reg_setnext_read_stage<0>(s0);
    Int<INDEX_WIDTH> index = addr.at<INDEX_WIDTH - 1, 0>();
    __vul_bram_readreq_tag_array<0>(index);
    __vul_bram_readreq_data_array<0>(index);
    read_inputed = true;
};
auto tick0__ = [&]() {
    bool hit = false;
    Int<DATA_WIDTH> read_data;
    if (__vul_read_reg_read_stage().valid) {
        Int<TAG_WIDTH> tag = __vul_read_reg_read_stage().addr.at<ADDR_WIDTH - 1, INDEX_WIDTH>();
        TagEntry tag_entry = __vul_bram_readdata_tag_array<0>();
        Int<TAG_WIDTH> read_tag = tag_entry.tag;
        bool valid = tag_entry.valid;
        if (valid && read_tag == tag) {
            hit = true;
            read_data = __vul_bram_readdata_data_array<0>();
        }
        __vul_req_call_readresp_s1(hit, read_data);
    }
    if (!read_inputed) {
        ReadStageReg s0;
        s0.valid = false;
        __vul_reg_setnext_read_stage<0>(s0);
    }
};

{
  Int<32> addr = {};
  addr = Int<32>(refill_s0_addr__.at<31, 0>());
  Int<64> data = {};
  data = Int<64>(refill_s0_data__.at<63, 0>());
  if (refill_s0__vld__) {
    refill_s0_impl__(addr, data);
  }
}
{
  Int<32> addr = {};
  addr = Int<32>(read_s0_addr__.at<31, 0>());
  if (read_s0__vld__) {
    read_s0_impl__(addr);
  }
}
tick0__();

}
