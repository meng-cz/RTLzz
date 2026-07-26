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

#define lane0_in(data) { \
  lane__0_left_in__vld__ = true; \
  lane__0_left_in_data__.at<31, 0>() = Int<32>(data); \
}

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
#pragma output_port lane__0_left_in__vld__
bool  lane__0_left_in__vld__;
#pragma output_port lane__0_left_in_data__
Int<32>  lane__0_left_in_data__;

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

void LogicSubModule_Top_top() {
constexpr int64_t LEN = 4;

holdnext_cycle__ = false;
resetnext_cycle__ = false;
wen_cycle__ = false;
wdata_cycle__ = 0;
lane__0_left_in__vld__ = false;

{
  uint32_t cycle;
    cycle = 0;
  Int<32> cycle_flatten = 0;
  cycle_flatten.at<31, 0>() = Int<32>(cycle);
  resetvalue_cycle__ = cycle_flatten;
}

auto tick0__ = [&]() {
    if (__vul_read_reg_cycle() == 0) {
        lane0_in(5);
    }
    __vul_reg_setnext_cycle<0>(__vul_read_reg_cycle() + 1);
};

tick0__();

#undef lane0_in
}
