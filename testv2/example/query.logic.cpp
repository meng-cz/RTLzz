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

struct ChildSnapshot {
    uint32_t sum;
    bool can_pop;
};

struct TopSnapshot {
    ChildSnapshot child;
    uint32_t mirror;
    bool mirror_even;
};

#define node_push(data) { \
  node_push__vld__ = true; \
  node_push_data__.at<31, 0>() = Int<32>(data); \
}

#pragma input_port rdata_mirror__
Int<32> rdata_mirror__;
#pragma output_port wen_mirror__
bool wen_mirror__;
#pragma output_port wdata_mirror__
Int<32> wdata_mirror__;
#pragma output_port holdnext_mirror__
bool holdnext_mirror__;
#pragma output_port resetnext_mirror__
bool resetnext_mirror__;
#pragma output_port resetvalue_mirror__
Int<32> resetvalue_mirror__;
#pragma output_port output__vld__
bool output__vld__;
#pragma output_port output_data__
Int<32> output_data__;
#pragma output_port snapshot__query__
Int<66> snapshot__query__;
#pragma output_port node_push__vld__
bool  node_push__vld__;
#pragma output_port node_push_data__
Int<32>  node_push_data__;
#pragma input_port node_snapshot__query__
Int<33> node_snapshot__query__;

uint32_t __vul_read_reg_mirror() {
  const Int<32> &__vul_rdata = rdata_mirror__;
  uint32_t value = 0;
  value = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_mirror(uint32_t value) {
  Int<32> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value);
  wdata_mirror__ = __vul_reg_wdata;
  wen_mirror__ = true;
}
void __vul_reg_holdnext_mirror() {
  holdnext_mirror__ = true;
}
void __vul_reg_resetnext_mirror() {
  resetnext_mirror__ = true;
}

void __vul_req_call_output(uint32_t data) {
  output__vld__ = true;
  uint32_t __vul_req_arg_data = (data);
  output_data__.at<31, 0>() = Int<32>(__vul_req_arg_data);
}

void LogicSubModule_Top_top() {
holdnext_mirror__ = false;
resetnext_mirror__ = false;
wen_mirror__ = false;
wdata_mirror__ = 0;
output__vld__ = false;
output_data__ = 0;
node_push__vld__ = false;

auto node_snapshot = [&]() -> ChildSnapshot {
  ChildSnapshot value = {};
  value.sum = Int<32>(node_snapshot__query__.at<31, 0>()).template to<typename std::remove_reference<decltype(value.sum)>::type>();
  value.can_pop = ReduceOr(node_snapshot__query__.at<32, 32>());
  return value;
};

{
  uint32_t mirror;
    mirror = 0;
  Int<32> mirror_flatten = 0;
  mirror_flatten.at<31, 0>() = Int<32>(mirror);
  resetvalue_mirror__ = mirror_flatten;
}

auto snapshot = [&]() -> TopSnapshot {
    TopSnapshot value;
    value.child = node_snapshot();
    value.mirror = __vul_read_reg_mirror();
    value.mirror_even = ((__vul_read_reg_mirror() & 1U) == 0U);
    return value;
};
auto tick0__ = [&]() {
    TopSnapshot snap = snapshot();
    if (snap.child.can_pop && snap.mirror_even) {
        __vul_req_call_output(snap.child.sum);
    }
    node_push(3);
    __vul_reg_setnext_mirror<0>(snap.child.sum);
};

{
  TopSnapshot value = snapshot();
  Int<66> packed = 0;
  packed.at<31, 0>() = Int<32>(value.child.sum);
  packed.at<32, 32>() = Int<1>(value.child.can_pop);
  packed.at<64, 33>() = Int<32>(value.mirror);
  packed.at<65, 65>() = Int<1>(value.mirror_even);
  snapshot__query__ = packed;
}
tick0__();

#undef node_push
}

