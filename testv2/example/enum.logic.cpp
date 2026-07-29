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

enum class CoreState {
    RESET = 1,
    RUNNING,
    WAITING = 7,
    HALTED,
};

struct StatusSnapshot {
    CoreState state;
    uint32_t code;
};

inline uint32_t passthrough_code(uint32_t value) {
    return value;
}


#pragma input_port rdata_state__
Int<2> rdata_state__;
#pragma output_port wen_state__
bool wen_state__;
#pragma output_port wdata_state__
Int<2> wdata_state__;
#pragma output_port holdnext_state__
bool holdnext_state__;
#pragma output_port resetnext_state__
bool resetnext_state__;
#pragma output_port resetvalue_state__
Int<2> resetvalue_state__;
#pragma input_port rdata_code__
Int<32> rdata_code__;
#pragma output_port wen_code__
bool wen_code__;
#pragma output_port wdata_code__
Int<32> wdata_code__;
#pragma output_port holdnext_code__
bool holdnext_code__;
#pragma output_port resetnext_code__
bool resetnext_code__;
#pragma output_port resetvalue_code__
Int<32> resetvalue_code__;
#pragma output_port snapshot__query__
Int<34> snapshot__query__;
#pragma output_port current_state__query__
Int<2> current_state__query__;
#pragma input_port set_state__vld__
bool set_state__vld__;
#pragma input_port set_state_next_state__
Int<2> set_state_next_state__;
#pragma input_port set_state_next_code__
Int<32> set_state_next_code__;

uint32_t __vul_read_reg_code() {
  const Int<32> &__vul_rdata = rdata_code__;
  uint32_t value = 0;
  value = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_code(uint32_t value) {
  Int<32> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value);
  wdata_code__ = __vul_reg_wdata;
  wen_code__ = true;
}
void __vul_reg_holdnext_code() {
  holdnext_code__ = true;
}
void __vul_reg_resetnext_code() {
  resetnext_code__ = true;
}

CoreState __vul_read_reg_state() {
  const Int<2> &__vul_rdata = rdata_state__;
  CoreState value = CoreState::RESET;
  value = static_cast<CoreState>(Int<2>(__vul_rdata.at<1, 0>()).template to<uint64_t>());
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_state(CoreState value) {
  Int<2> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<1, 0>() = Int<2>(static_cast<uint64_t>(value));
  wdata_state__ = __vul_reg_wdata;
  wen_state__ = true;
}
void __vul_reg_holdnext_state() {
  holdnext_state__ = true;
}
void __vul_reg_resetnext_state() {
  resetnext_state__ = true;
}

void LogicSubModule_Top_top() {
holdnext_state__ = false;
resetnext_state__ = false;
wen_state__ = false;
wdata_state__ = 0;
holdnext_code__ = false;
resetnext_code__ = false;
wen_code__ = false;
wdata_code__ = 0;

{
  CoreState state;
    state = CoreState::RESET;
  Int<2> state_flatten = 0;
  state_flatten.at<1, 0>() = Int<2>(static_cast<uint64_t>(state));
  resetvalue_state__ = state_flatten;
}
{
  uint32_t code;
    code = 0;
  Int<32> code_flatten = 0;
  code_flatten.at<31, 0>() = Int<32>(code);
  resetvalue_code__ = code_flatten;
}

auto snapshot = [&]() -> StatusSnapshot {
    StatusSnapshot s;
    s.state = __vul_read_reg_state();
    s.code = passthrough_code(__vul_read_reg_code());
    return s;
};
auto current_state = [&]() -> CoreState {
    return __vul_read_reg_state();
};
auto set_state_impl__ = [&](CoreState next_state, uint32_t next_code) -> void {
    __vul_reg_setnext_state<0>(next_state);
    __vul_reg_setnext_code<0>(passthrough_code(next_code));
};

{
  StatusSnapshot value = snapshot();
  Int<34> packed = 0;
  packed.at<1, 0>() = Int<2>(static_cast<uint64_t>(value.state));
  packed.at<33, 2>() = Int<32>(value.code);
  snapshot__query__ = packed;
}
{
  CoreState value = current_state();
  Int<2> packed = 0;
  packed.at<1, 0>() = Int<2>(static_cast<uint64_t>(value));
  current_state__query__ = packed;
}
{
  CoreState next_state = CoreState::RESET;
  next_state = static_cast<CoreState>(Int<2>(set_state_next_state__.at<1, 0>()).template to<uint64_t>());
  uint32_t next_code = {};
  next_code = Int<32>(set_state_next_code__.at<31, 0>()).template to<typename std::remove_reference<decltype(next_code)>::type>();
  if (set_state__vld__) {
    set_state_impl__(next_state, next_code);
  }
}

}

