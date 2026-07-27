#include <array>
#include <cstdint>
#include <type_traits>
#include <fixint.hpp>

using std::array;

using int8 = int8_t;
using uint8 = uint8_t;
using int16 = int16_t;
using uint16 = uint16_t;
using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;

#define HEI (2)
#define WID (2)


#pragma input_port rdata_cycle__
Int<32> rdata_cycle__;
#pragma output_port wen_cycle__
bool wen_cycle__;
#pragma output_port wdata_cycle__
Int<32> wdata_cycle__;
#pragma output_port holdnext_cycle__
bool holdnext_cycle__;
#pragma output_port resetnext_cycle__
bool resetnext_cycle__;
#pragma output_port resetvalue_cycle__
Int<32> resetvalue_cycle__;
#pragma input_port rdata_captured0__
Int<32> rdata_captured0__;
#pragma output_port wen_captured0__
bool wen_captured0__;
#pragma output_port wdata_captured0__
Int<32> wdata_captured0__;
#pragma output_port holdnext_captured0__
bool holdnext_captured0__;
#pragma output_port resetnext_captured0__
bool resetnext_captured0__;
#pragma output_port resetvalue_captured0__
Int<32> resetvalue_captured0__;
#pragma input_port rdata_captured1__
Int<32> rdata_captured1__;
#pragma output_port wen_captured1__
bool wen_captured1__;
#pragma output_port wdata_captured1__
Int<32> wdata_captured1__;
#pragma output_port holdnext_captured1__
bool holdnext_captured1__;
#pragma output_port resetnext_captured1__
bool resetnext_captured1__;
#pragma output_port resetvalue_captured1__
Int<32> resetvalue_captured1__;
#pragma output_port print__vld__
std::array<bool, 2> print__vld__;
#pragma output_port print_data__
std::array<Int<32>, 2> print_data__;
#pragma input_port output__vld__
std::array<bool, 2> output__vld__;
#pragma input_port output_data__
std::array<Int<32>, 2> output_data__;
#pragma output_port mesh__0__0_left_in__vld__
bool  mesh__0__0_left_in__vld__;
#pragma output_port mesh__0__0_left_in_data__
Int<32>  mesh__0__0_left_in_data__;
#pragma output_port mesh__1__0_left_in__vld__
bool  mesh__1__0_left_in__vld__;
#pragma output_port mesh__1__0_left_in_data__
Int<32>  mesh__1__0_left_in_data__;

uint32_t __vul_read_reg_captured1() {
  const Int<32> &__vul_rdata = rdata_captured1__;
  uint32_t value = 0;
  value = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_captured1(uint32_t value) {
  Int<32> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value);
  wdata_captured1__ = __vul_reg_wdata;
  wen_captured1__ = true;
}
void __vul_reg_holdnext_captured1() {
  holdnext_captured1__ = true;
}
void __vul_reg_resetnext_captured1() {
  resetnext_captured1__ = true;
}

uint32_t __vul_read_reg_captured0() {
  const Int<32> &__vul_rdata = rdata_captured0__;
  uint32_t value = 0;
  value = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_captured0(uint32_t value) {
  Int<32> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value);
  wdata_captured0__ = __vul_reg_wdata;
  wen_captured0__ = true;
}
void __vul_reg_holdnext_captured0() {
  holdnext_captured0__ = true;
}
void __vul_reg_resetnext_captured0() {
  resetnext_captured0__ = true;
}

uint32_t __vul_read_reg_cycle() {
  const Int<32> &__vul_rdata = rdata_cycle__;
  uint32_t value = 0;
  value = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_cycle(uint32_t value) {
  Int<32> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value);
  wdata_cycle__ = __vul_reg_wdata;
  wen_cycle__ = true;
}
void __vul_reg_holdnext_cycle() {
  holdnext_cycle__ = true;
}
void __vul_reg_resetnext_cycle() {
  resetnext_cycle__ = true;
}

template <uint32_t IDX = 0>
void __vul_req_call_print(uint32_t data) {
  print__vld__[IDX] = true;
  uint32_t __vul_req_arg_data = (data);
  print_data__[IDX].at<31, 0>() = Int<32>(__vul_req_arg_data);
}

void LogicSubModule_Top_top() {
holdnext_cycle__ = false;
resetnext_cycle__ = false;
wen_cycle__ = false;
wdata_cycle__ = 0;
holdnext_captured0__ = false;
resetnext_captured0__ = false;
wen_captured0__ = false;
wdata_captured0__ = 0;
holdnext_captured1__ = false;
resetnext_captured1__ = false;
wen_captured1__ = false;
wdata_captured1__ = 0;
print__vld__[0] = false;
print__vld__[1] = false;
print_data__[0] = 0;
print_data__[1] = 0;
mesh__0__0_left_in__vld__ = false;
mesh__1__0_left_in__vld__ = false;

auto mesh_in = [&]<uint32_t IDX = 0>(uint32_t data) -> void {
  static_assert(IDX < 2, "Implicit request index out of range");
  if constexpr (IDX == 0) {
    mesh__0__0_left_in__vld__ = true;
    mesh__0__0_left_in_data__.at<31, 0>() = Int<32>(data);
    return;
  }
  else if constexpr (IDX == 1) {
    mesh__1__0_left_in__vld__ = true;
    mesh__1__0_left_in_data__.at<31, 0>() = Int<32>(data);
    return;
  }
};

{
  uint32_t cycle;
    cycle = 0;
  Int<32> cycle_flatten = 0;
  cycle_flatten.at<31, 0>() = Int<32>(cycle);
  resetvalue_cycle__ = cycle_flatten;
}
{
  uint32_t captured0;
    captured0 = 0;
  Int<32> captured0_flatten = 0;
  captured0_flatten.at<31, 0>() = Int<32>(captured0);
  resetvalue_captured0__ = captured0_flatten;
}
{
  uint32_t captured1;
    captured1 = 0;
  Int<32> captured1_flatten = 0;
  captured1_flatten.at<31, 0>() = Int<32>(captured1);
  resetvalue_captured1__ = captured1_flatten;
}

auto output_impl__ = [&]<uint32_t IDX = 0>(uint32_t data) -> void {
    if constexpr (IDX == 0) {
        __vul_reg_setnext_captured0<0>(data);
    } else if constexpr (IDX == 1) {
        __vul_reg_setnext_captured1<0>(data);
    }
};
auto tick0__ = [&]() {
    if (__vul_read_reg_cycle() == 0) {
        mesh_in.template operator()<0>(1);
        mesh_in.template operator()<1>(10);
    }
    if (__vul_read_reg_cycle() == 1) {
        __vul_req_call_print<0>(__vul_read_reg_captured0());
        __vul_req_call_print<1>(__vul_read_reg_captured1());
    }
    __vul_reg_setnext_cycle<0>(__vul_read_reg_cycle() + 1);
};

{
  uint32_t data = {};
  data = Int<32>(output_data__[0].at<31, 0>()).template to<typename std::remove_reference<decltype(data)>::type>();
  if (output__vld__[0]) {
    output_impl__.template operator()<0>(data);
  }
}
{
  uint32_t data = {};
  data = Int<32>(output_data__[1].at<31, 0>()).template to<typename std::remove_reference<decltype(data)>::type>();
  if (output__vld__[1]) {
    output_impl__.template operator()<1>(data);
  }
}
tick0__();

}

