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

struct MetaInfo {
    uint8_t tag;
    bool valid;
};

struct Payload {
    uint32_t data;
    MetaInfo meta;
    bool flag;
};

struct RegisterSummary {
    uint32_t scalar;
    uint32_t multi;
    uint16_t arr0;
    uint32_t payload_data;
    uint8_t payload_tag;
    bool payload_valid;
};


#pragma input_port rdata_scalar_reg__
Int<32> rdata_scalar_reg__;
#pragma output_port wen_scalar_reg__
bool wen_scalar_reg__;
#pragma output_port wdata_scalar_reg__
Int<32> wdata_scalar_reg__;
#pragma output_port holdnext_scalar_reg__
bool holdnext_scalar_reg__;
#pragma output_port resetnext_scalar_reg__
bool resetnext_scalar_reg__;
#pragma output_port resetvalue_scalar_reg__
Int<32> resetvalue_scalar_reg__;
#pragma input_port rdata_multi_reg__
Int<32> rdata_multi_reg__;
#pragma output_port wen_multi_reg__
std::array<bool, 3> wen_multi_reg__;
#pragma output_port wdata_multi_reg__
std::array<Int<32>, 3> wdata_multi_reg__;
#pragma output_port holdnext_multi_reg__
bool holdnext_multi_reg__;
#pragma output_port resetnext_multi_reg__
bool resetnext_multi_reg__;
#pragma output_port resetvalue_multi_reg__
Int<32> resetvalue_multi_reg__;
#pragma input_port rdata_array_reg__
std::array<Int<16>, 4> rdata_array_reg__;
#pragma output_port wen_array_reg__
std::array<std::array<bool, 2>, 4> wen_array_reg__;
#pragma output_port wdata_array_reg__
std::array<std::array<Int<16>, 2>, 4> wdata_array_reg__;
#pragma output_port holdnext_array_reg__
std::array<bool, 4> holdnext_array_reg__;
#pragma output_port resetnext_array_reg__
std::array<bool, 4> resetnext_array_reg__;
#pragma output_port resetvalue_array_reg__
std::array<Int<16>, 4> resetvalue_array_reg__;
#pragma input_port rdata_payload_reg__
Int<42> rdata_payload_reg__;
#pragma output_port wen_payload_reg__
bool wen_payload_reg__;
#pragma output_port wdata_payload_reg__
Int<42> wdata_payload_reg__;
#pragma output_port holdnext_payload_reg__
bool holdnext_payload_reg__;
#pragma output_port resetnext_payload_reg__
bool resetnext_payload_reg__;
#pragma output_port resetvalue_payload_reg__
Int<42> resetvalue_payload_reg__;
#pragma input_port rdata_payload_array__
std::array<Int<42>, 4> rdata_payload_array__;
#pragma output_port wen_payload_array__
std::array<std::array<bool, 2>, 4> wen_payload_array__;
#pragma output_port wdata_payload_array__
std::array<std::array<Int<42>, 2>, 4> wdata_payload_array__;
#pragma output_port holdnext_payload_array__
std::array<bool, 4> holdnext_payload_array__;
#pragma output_port resetnext_payload_array__
std::array<bool, 4> resetnext_payload_array__;
#pragma output_port resetvalue_payload_array__
std::array<Int<42>, 4> resetvalue_payload_array__;
#pragma output_port summary__query__
Int<121> summary__query__;
#pragma input_port write_payload__vld__
bool write_payload__vld__;
#pragma input_port write_payload_next_payload__
Int<42> write_payload_next_payload__;
#pragma input_port write_scalar__vld__
bool write_scalar__vld__;
#pragma input_port write_scalar_value__
Int<32> write_scalar_value__;

Payload __vul_read_reg_payload_array(uint32_t idx) {
  const Int<42> &__vul_rdata = rdata_payload_array__[idx];
  Payload value = {};
  value.data = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value.data)>::type>();
  value.meta.tag = Int<8>(__vul_rdata.at<39, 32>()).template to<typename std::remove_reference<decltype(value.meta.tag)>::type>();
  value.meta.valid = ReduceOr(__vul_rdata.at<40, 40>());
  value.flag = ReduceOr(__vul_rdata.at<41, 41>());
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_payload_array(uint32_t __vul_reg_idx, Payload value) {
  Int<42> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value.data);
  __vul_reg_wdata.at<39, 32>() = Int<8>(value.meta.tag);
  __vul_reg_wdata.at<40, 40>() = Int<1>(value.meta.valid);
  __vul_reg_wdata.at<41, 41>() = Int<1>(value.flag);
  wdata_payload_array__[__vul_reg_idx][P] = __vul_reg_wdata;
  wen_payload_array__[__vul_reg_idx][P] = true;
}
void __vul_reg_holdnext_payload_array(uint32_t __vul_reg_idx) {
  holdnext_payload_array__[__vul_reg_idx] = true;
}
void __vul_reg_holdnext_payload_array() {
  for (uint32_t __vul_i = 0; __vul_i < 4; ++__vul_i) {
    holdnext_payload_array__[__vul_i] = true;
  }
}
void __vul_reg_resetnext_payload_array(uint32_t __vul_reg_idx) {
  resetnext_payload_array__[__vul_reg_idx] = true;
}
void __vul_reg_resetnext_payload_array() {
  for (uint32_t __vul_i = 0; __vul_i < 4; ++__vul_i) {
    resetnext_payload_array__[__vul_i] = true;
  }
}

Payload __vul_read_reg_payload_reg() {
  const Int<42> &__vul_rdata = rdata_payload_reg__;
  Payload value = {};
  value.data = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value.data)>::type>();
  value.meta.tag = Int<8>(__vul_rdata.at<39, 32>()).template to<typename std::remove_reference<decltype(value.meta.tag)>::type>();
  value.meta.valid = ReduceOr(__vul_rdata.at<40, 40>());
  value.flag = ReduceOr(__vul_rdata.at<41, 41>());
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_payload_reg(Payload value) {
  Int<42> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value.data);
  __vul_reg_wdata.at<39, 32>() = Int<8>(value.meta.tag);
  __vul_reg_wdata.at<40, 40>() = Int<1>(value.meta.valid);
  __vul_reg_wdata.at<41, 41>() = Int<1>(value.flag);
  wdata_payload_reg__ = __vul_reg_wdata;
  wen_payload_reg__ = true;
}
void __vul_reg_holdnext_payload_reg() {
  holdnext_payload_reg__ = true;
}
void __vul_reg_resetnext_payload_reg() {
  resetnext_payload_reg__ = true;
}

uint16_t __vul_read_reg_array_reg(uint32_t idx) {
  const Int<16> &__vul_rdata = rdata_array_reg__[idx];
  uint16_t value = 0;
  value = Int<16>(__vul_rdata.at<15, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_array_reg(uint32_t __vul_reg_idx, uint16_t value) {
  Int<16> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<15, 0>() = Int<16>(value);
  wdata_array_reg__[__vul_reg_idx][P] = __vul_reg_wdata;
  wen_array_reg__[__vul_reg_idx][P] = true;
}
void __vul_reg_holdnext_array_reg(uint32_t __vul_reg_idx) {
  holdnext_array_reg__[__vul_reg_idx] = true;
}
void __vul_reg_holdnext_array_reg() {
  for (uint32_t __vul_i = 0; __vul_i < 4; ++__vul_i) {
    holdnext_array_reg__[__vul_i] = true;
  }
}
void __vul_reg_resetnext_array_reg(uint32_t __vul_reg_idx) {
  resetnext_array_reg__[__vul_reg_idx] = true;
}
void __vul_reg_resetnext_array_reg() {
  for (uint32_t __vul_i = 0; __vul_i < 4; ++__vul_i) {
    resetnext_array_reg__[__vul_i] = true;
  }
}

uint32_t __vul_read_reg_multi_reg() {
  const Int<32> &__vul_rdata = rdata_multi_reg__;
  uint32_t value = 0;
  value = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_multi_reg(uint32_t value) {
  Int<32> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value);
  wdata_multi_reg__[P] = __vul_reg_wdata;
  wen_multi_reg__[P] = true;
}
void __vul_reg_holdnext_multi_reg() {
  holdnext_multi_reg__ = true;
}
void __vul_reg_resetnext_multi_reg() {
  resetnext_multi_reg__ = true;
}

uint32_t __vul_read_reg_scalar_reg() {
  const Int<32> &__vul_rdata = rdata_scalar_reg__;
  uint32_t value = 0;
  value = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_scalar_reg(uint32_t value) {
  Int<32> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value);
  wdata_scalar_reg__ = __vul_reg_wdata;
  wen_scalar_reg__ = true;
}
void __vul_reg_holdnext_scalar_reg() {
  holdnext_scalar_reg__ = true;
}
void __vul_reg_resetnext_scalar_reg() {
  resetnext_scalar_reg__ = true;
}

void LogicSubModule_Top_top() {
constexpr int64_t REG_ARRAY_SIZE = 4;

holdnext_scalar_reg__ = false;
resetnext_scalar_reg__ = false;
wen_scalar_reg__ = false;
wdata_scalar_reg__ = 0;
holdnext_multi_reg__ = false;
resetnext_multi_reg__ = false;
for (uint32_t __vul_p = 0; __vul_p < 3; ++__vul_p) {
  wen_multi_reg__[__vul_p] = false;
  wdata_multi_reg__[__vul_p] = 0;
}
for (uint32_t __vul_i = 0; __vul_i < 4; ++__vul_i) {
  holdnext_array_reg__[__vul_i] = false;
  resetnext_array_reg__[__vul_i] = false;
  for (uint32_t __vul_p = 0; __vul_p < 2; ++__vul_p) {
    wen_array_reg__[__vul_i][__vul_p] = false;
    wdata_array_reg__[__vul_i][__vul_p] = 0;
  }
}
holdnext_payload_reg__ = false;
resetnext_payload_reg__ = false;
wen_payload_reg__ = false;
wdata_payload_reg__ = 0;
for (uint32_t __vul_i = 0; __vul_i < 4; ++__vul_i) {
  holdnext_payload_array__[__vul_i] = false;
  resetnext_payload_array__[__vul_i] = false;
  for (uint32_t __vul_p = 0; __vul_p < 2; ++__vul_p) {
    wen_payload_array__[__vul_i][__vul_p] = false;
    wdata_payload_array__[__vul_i][__vul_p] = 0;
  }
}

{
  uint32_t scalar_reg;
    scalar_reg = 1;
  Int<32> scalar_reg_flatten = 0;
  scalar_reg_flatten.at<31, 0>() = Int<32>(scalar_reg);
  resetvalue_scalar_reg__ = scalar_reg_flatten;
}
{
  uint32_t multi_reg;
    multi_reg = 10;
  Int<32> multi_reg_flatten = 0;
  multi_reg_flatten.at<31, 0>() = Int<32>(multi_reg);
  resetvalue_multi_reg__ = multi_reg_flatten;
}
{
  std::array<uint16_t, 4> array_reg;
    for (int i = 0; i < 4; ++i) {
        array_reg[i] = static_cast<uint16_t>(i);
    }
  Int<16> array_reg_flatten_0 = 0;
  array_reg_flatten_0.at<15, 0>() = Int<16>(array_reg[0]);
  resetvalue_array_reg__[0] = array_reg_flatten_0;
  Int<16> array_reg_flatten_1 = 0;
  array_reg_flatten_1.at<15, 0>() = Int<16>(array_reg[1]);
  resetvalue_array_reg__[1] = array_reg_flatten_1;
  Int<16> array_reg_flatten_2 = 0;
  array_reg_flatten_2.at<15, 0>() = Int<16>(array_reg[2]);
  resetvalue_array_reg__[2] = array_reg_flatten_2;
  Int<16> array_reg_flatten_3 = 0;
  array_reg_flatten_3.at<15, 0>() = Int<16>(array_reg[3]);
  resetvalue_array_reg__[3] = array_reg_flatten_3;
}
{
  Payload payload_reg;
    payload_reg.data = 0;
    payload_reg.meta.tag = 3;
    payload_reg.meta.valid = false;
    payload_reg.flag = false;
  Int<42> payload_reg_flatten = 0;
  payload_reg_flatten.at<31, 0>() = Int<32>(payload_reg.data);
  payload_reg_flatten.at<39, 32>() = Int<8>(payload_reg.meta.tag);
  payload_reg_flatten.at<40, 40>() = Int<1>(payload_reg.meta.valid);
  payload_reg_flatten.at<41, 41>() = Int<1>(payload_reg.flag);
  resetvalue_payload_reg__ = payload_reg_flatten;
}
{
  std::array<Payload, 4> payload_array;
    for (int i = 0; i < 4; ++i) {
        payload_array[i].data = static_cast<uint32_t>(i);
        payload_array[i].meta.tag = static_cast<uint8_t>(i + 1);
        payload_array[i].meta.valid = (i == 0);
        payload_array[i].flag = false;
    }
  Int<42> payload_array_flatten_0 = 0;
  payload_array_flatten_0.at<31, 0>() = Int<32>(payload_array[0].data);
  payload_array_flatten_0.at<39, 32>() = Int<8>(payload_array[0].meta.tag);
  payload_array_flatten_0.at<40, 40>() = Int<1>(payload_array[0].meta.valid);
  payload_array_flatten_0.at<41, 41>() = Int<1>(payload_array[0].flag);
  resetvalue_payload_array__[0] = payload_array_flatten_0;
  Int<42> payload_array_flatten_1 = 0;
  payload_array_flatten_1.at<31, 0>() = Int<32>(payload_array[1].data);
  payload_array_flatten_1.at<39, 32>() = Int<8>(payload_array[1].meta.tag);
  payload_array_flatten_1.at<40, 40>() = Int<1>(payload_array[1].meta.valid);
  payload_array_flatten_1.at<41, 41>() = Int<1>(payload_array[1].flag);
  resetvalue_payload_array__[1] = payload_array_flatten_1;
  Int<42> payload_array_flatten_2 = 0;
  payload_array_flatten_2.at<31, 0>() = Int<32>(payload_array[2].data);
  payload_array_flatten_2.at<39, 32>() = Int<8>(payload_array[2].meta.tag);
  payload_array_flatten_2.at<40, 40>() = Int<1>(payload_array[2].meta.valid);
  payload_array_flatten_2.at<41, 41>() = Int<1>(payload_array[2].flag);
  resetvalue_payload_array__[2] = payload_array_flatten_2;
  Int<42> payload_array_flatten_3 = 0;
  payload_array_flatten_3.at<31, 0>() = Int<32>(payload_array[3].data);
  payload_array_flatten_3.at<39, 32>() = Int<8>(payload_array[3].meta.tag);
  payload_array_flatten_3.at<40, 40>() = Int<1>(payload_array[3].meta.valid);
  payload_array_flatten_3.at<41, 41>() = Int<1>(payload_array[3].flag);
  resetvalue_payload_array__[3] = payload_array_flatten_3;
}

auto summary = [&]() -> RegisterSummary {
    RegisterSummary result;
    result.scalar = __vul_read_reg_scalar_reg();
    result.multi = __vul_read_reg_multi_reg();
    result.arr0 = __vul_read_reg_array_reg(0);
    result.payload_data = __vul_read_reg_payload_reg().data;
    result.payload_tag = __vul_read_reg_payload_array(1).meta.tag;
    result.payload_valid = __vul_read_reg_payload_array(2).meta.valid;
    return result;
};
auto write_payload_impl__ = [&](Payload next_payload) -> void {
    __vul_reg_setnext_payload_reg<0>(next_payload);
};
auto write_scalar_impl__ = [&](uint32_t value) -> void {
    __vul_reg_setnext_scalar_reg<0>(value);
};
auto tick0__ = [&]() {
    uint32_t idx = __vul_read_reg_scalar_reg() & (REG_ARRAY_SIZE - 1);
    uint32_t scalar_now = __vul_read_reg_scalar_reg();
    __vul_reg_setnext_multi_reg<0>(scalar_now + 1);
    __vul_reg_setnext_multi_reg<1>(__vul_read_reg_multi_reg() + 2);
    __vul_reg_setnext_multi_reg<2>(__vul_read_reg_array_reg(idx) + __vul_read_reg_multi_reg());
    __vul_reg_setnext_array_reg<0>(idx, static_cast<uint16_t>(__vul_read_reg_array_reg(idx) + __vul_read_reg_scalar_reg()));
    __vul_reg_setnext_array_reg<1>((idx + 1) & (REG_ARRAY_SIZE - 1), static_cast<uint16_t>(__vul_read_reg_multi_reg()));
    Payload payload_next = __vul_read_reg_payload_reg();
    payload_next.data = __vul_read_reg_payload_reg().data + __vul_read_reg_scalar_reg();
    payload_next.meta.tag = static_cast<uint8_t>(__vul_read_reg_payload_array(idx).meta.tag + 1);
    payload_next.meta.valid = !payload_next.meta.valid;
    payload_next.flag = !payload_next.flag;
    __vul_reg_setnext_payload_reg<0>(payload_next);
    Payload array_payload_next = __vul_read_reg_payload_array(idx);
    array_payload_next.data = __vul_read_reg_multi_reg() + __vul_read_reg_array_reg(idx);
    array_payload_next.meta.valid = true;
    __vul_reg_setnext_payload_array<1>(idx, array_payload_next);
};

{
  RegisterSummary value = summary();
  Int<121> packed = 0;
  packed.at<31, 0>() = Int<32>(value.scalar);
  packed.at<63, 32>() = Int<32>(value.multi);
  packed.at<79, 64>() = Int<16>(value.arr0);
  packed.at<111, 80>() = Int<32>(value.payload_data);
  packed.at<119, 112>() = Int<8>(value.payload_tag);
  packed.at<120, 120>() = Int<1>(value.payload_valid);
  summary__query__ = packed;
}
{
  Payload next_payload = {};
  next_payload.data = Int<32>(write_payload_next_payload__.at<31, 0>()).template to<typename std::remove_reference<decltype(next_payload.data)>::type>();
  next_payload.meta.tag = Int<8>(write_payload_next_payload__.at<39, 32>()).template to<typename std::remove_reference<decltype(next_payload.meta.tag)>::type>();
  next_payload.meta.valid = ReduceOr(write_payload_next_payload__.at<40, 40>());
  next_payload.flag = ReduceOr(write_payload_next_payload__.at<41, 41>());
  if (write_payload__vld__) {
    write_payload_impl__(next_payload);
  }
}
{
  uint32_t value = {};
  value = Int<32>(write_scalar_value__.at<31, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  if (write_scalar__vld__) {
    write_scalar_impl__(value);
  }
}
tick0__();

}
