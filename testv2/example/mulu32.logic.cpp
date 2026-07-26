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

struct S0S1RegData {
    uint32_t a;
    uint32_t b;
};

struct S1S2RegData {
    Int<32> p0;
    Int<32> p1;
    Int<32> p2;
    Int<32> p3;
};

struct S2S3RegData {
    Int<16> low16;
    Int<34> mid;
    Int<32> high_base;
};


#pragma input_port rdata_s1valid__
Int<1> rdata_s1valid__;
#pragma output_port wen_s1valid__
std::array<bool, 2> wen_s1valid__;
#pragma output_port wdata_s1valid__
std::array<Int<1>, 2> wdata_s1valid__;
#pragma output_port holdnext_s1valid__
bool holdnext_s1valid__;
#pragma output_port resetnext_s1valid__
bool resetnext_s1valid__;
#pragma output_port resetvalue_s1valid__
Int<1> resetvalue_s1valid__;
#pragma input_port rdata_s1reg__
Int<64> rdata_s1reg__;
#pragma output_port wen_s1reg__
bool wen_s1reg__;
#pragma output_port wdata_s1reg__
Int<64> wdata_s1reg__;
#pragma output_port holdnext_s1reg__
bool holdnext_s1reg__;
#pragma output_port resetnext_s1reg__
bool resetnext_s1reg__;
#pragma output_port resetvalue_s1reg__
Int<64> resetvalue_s1reg__;
#pragma input_port rdata_s2valid__
Int<1> rdata_s2valid__;
#pragma output_port wen_s2valid__
bool wen_s2valid__;
#pragma output_port wdata_s2valid__
Int<1> wdata_s2valid__;
#pragma output_port holdnext_s2valid__
bool holdnext_s2valid__;
#pragma output_port resetnext_s2valid__
bool resetnext_s2valid__;
#pragma output_port resetvalue_s2valid__
Int<1> resetvalue_s2valid__;
#pragma input_port rdata_s2reg__
Int<128> rdata_s2reg__;
#pragma output_port wen_s2reg__
bool wen_s2reg__;
#pragma output_port wdata_s2reg__
Int<128> wdata_s2reg__;
#pragma output_port holdnext_s2reg__
bool holdnext_s2reg__;
#pragma output_port resetnext_s2reg__
bool resetnext_s2reg__;
#pragma output_port resetvalue_s2reg__
Int<128> resetvalue_s2reg__;
#pragma input_port rdata_s3valid__
Int<1> rdata_s3valid__;
#pragma output_port wen_s3valid__
bool wen_s3valid__;
#pragma output_port wdata_s3valid__
Int<1> wdata_s3valid__;
#pragma output_port holdnext_s3valid__
bool holdnext_s3valid__;
#pragma output_port resetnext_s3valid__
bool resetnext_s3valid__;
#pragma output_port resetvalue_s3valid__
Int<1> resetvalue_s3valid__;
#pragma input_port rdata_s3reg__
Int<82> rdata_s3reg__;
#pragma output_port wen_s3reg__
bool wen_s3reg__;
#pragma output_port wdata_s3reg__
Int<82> wdata_s3reg__;
#pragma output_port holdnext_s3reg__
bool holdnext_s3reg__;
#pragma output_port resetnext_s3reg__
bool resetnext_s3reg__;
#pragma output_port resetvalue_s3reg__
Int<82> resetvalue_s3reg__;
#pragma output_port s3output__vld__
bool s3output__vld__;
#pragma output_port s3output_y__
Int<64> s3output_y__;
#pragma input_port s0input__vld__
bool s0input__vld__;
#pragma input_port s0input_a__
Int<32> s0input_a__;
#pragma input_port s0input_b__
Int<32> s0input_b__;

S2S3RegData __vul_read_reg_s3reg() {
  const Int<82> &__vul_rdata = rdata_s3reg__;
  S2S3RegData value = {};
  value.low16 = Int<16>(__vul_rdata.at<15, 0>());
  value.mid = Int<34>(__vul_rdata.at<49, 16>());
  value.high_base = Int<32>(__vul_rdata.at<81, 50>());
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_s3reg(S2S3RegData value) {
  Int<82> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<15, 0>() = Int<16>(value.low16);
  __vul_reg_wdata.at<49, 16>() = Int<34>(value.mid);
  __vul_reg_wdata.at<81, 50>() = Int<32>(value.high_base);
  wdata_s3reg__ = __vul_reg_wdata;
  wen_s3reg__ = true;
}
void __vul_reg_holdnext_s3reg() {
  holdnext_s3reg__ = true;
}
void __vul_reg_resetnext_s3reg() {
  resetnext_s3reg__ = true;
}

bool __vul_read_reg_s3valid() {
  const Int<1> &__vul_rdata = rdata_s3valid__;
  bool value = 0;
  value = ReduceOr(__vul_rdata.at<0, 0>());
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_s3valid(bool value) {
  Int<1> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<0, 0>() = Int<1>(value);
  wdata_s3valid__ = __vul_reg_wdata;
  wen_s3valid__ = true;
}
void __vul_reg_holdnext_s3valid() {
  holdnext_s3valid__ = true;
}
void __vul_reg_resetnext_s3valid() {
  resetnext_s3valid__ = true;
}

S1S2RegData __vul_read_reg_s2reg() {
  const Int<128> &__vul_rdata = rdata_s2reg__;
  S1S2RegData value = {};
  value.p0 = Int<32>(__vul_rdata.at<31, 0>());
  value.p1 = Int<32>(__vul_rdata.at<63, 32>());
  value.p2 = Int<32>(__vul_rdata.at<95, 64>());
  value.p3 = Int<32>(__vul_rdata.at<127, 96>());
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_s2reg(S1S2RegData value) {
  Int<128> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value.p0);
  __vul_reg_wdata.at<63, 32>() = Int<32>(value.p1);
  __vul_reg_wdata.at<95, 64>() = Int<32>(value.p2);
  __vul_reg_wdata.at<127, 96>() = Int<32>(value.p3);
  wdata_s2reg__ = __vul_reg_wdata;
  wen_s2reg__ = true;
}
void __vul_reg_holdnext_s2reg() {
  holdnext_s2reg__ = true;
}
void __vul_reg_resetnext_s2reg() {
  resetnext_s2reg__ = true;
}

S0S1RegData __vul_read_reg_s1reg() {
  const Int<64> &__vul_rdata = rdata_s1reg__;
  S0S1RegData value = {};
  value.a = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value.a)>::type>();
  value.b = Int<32>(__vul_rdata.at<63, 32>()).template to<typename std::remove_reference<decltype(value.b)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_s1reg(S0S1RegData value) {
  Int<64> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value.a);
  __vul_reg_wdata.at<63, 32>() = Int<32>(value.b);
  wdata_s1reg__ = __vul_reg_wdata;
  wen_s1reg__ = true;
}
void __vul_reg_holdnext_s1reg() {
  holdnext_s1reg__ = true;
}
void __vul_reg_resetnext_s1reg() {
  resetnext_s1reg__ = true;
}

bool __vul_read_reg_s2valid() {
  const Int<1> &__vul_rdata = rdata_s2valid__;
  bool value = 0;
  value = ReduceOr(__vul_rdata.at<0, 0>());
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_s2valid(bool value) {
  Int<1> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<0, 0>() = Int<1>(value);
  wdata_s2valid__ = __vul_reg_wdata;
  wen_s2valid__ = true;
}
void __vul_reg_holdnext_s2valid() {
  holdnext_s2valid__ = true;
}
void __vul_reg_resetnext_s2valid() {
  resetnext_s2valid__ = true;
}

bool __vul_read_reg_s1valid() {
  const Int<1> &__vul_rdata = rdata_s1valid__;
  bool value = 0;
  value = ReduceOr(__vul_rdata.at<0, 0>());
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_s1valid(bool value) {
  Int<1> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<0, 0>() = Int<1>(value);
  wdata_s1valid__[P] = __vul_reg_wdata;
  wen_s1valid__[P] = true;
}
void __vul_reg_holdnext_s1valid() {
  holdnext_s1valid__ = true;
}
void __vul_reg_resetnext_s1valid() {
  resetnext_s1valid__ = true;
}

void __vul_req_call_s3output(uint64_t y) {
  s3output__vld__ = true;
  uint64_t __vul_req_arg_y = (y);
  s3output_y__.at<63, 0>() = Int<64>(__vul_req_arg_y);
}

void LogicSubModule_MulU32_top() {
holdnext_s1valid__ = false;
resetnext_s1valid__ = false;
for (uint32_t __vul_p = 0; __vul_p < 2; ++__vul_p) {
  wen_s1valid__[__vul_p] = false;
  wdata_s1valid__[__vul_p] = 0;
}
holdnext_s1reg__ = false;
resetnext_s1reg__ = false;
wen_s1reg__ = false;
wdata_s1reg__ = 0;
holdnext_s2valid__ = false;
resetnext_s2valid__ = false;
wen_s2valid__ = false;
wdata_s2valid__ = 0;
holdnext_s2reg__ = false;
resetnext_s2reg__ = false;
wen_s2reg__ = false;
wdata_s2reg__ = 0;
holdnext_s3valid__ = false;
resetnext_s3valid__ = false;
wen_s3valid__ = false;
wdata_s3valid__ = 0;
holdnext_s3reg__ = false;
resetnext_s3reg__ = false;
wen_s3reg__ = false;
wdata_s3reg__ = 0;
s3output__vld__ = false;
s3output_y__ = 0;

{
  bool s1valid;
    s1valid = false;
  Int<1> s1valid_flatten = 0;
  s1valid_flatten.at<0, 0>() = Int<1>(s1valid);
  resetvalue_s1valid__ = s1valid_flatten;
}
{
  S0S1RegData s1reg;
    s1reg.a = 0;
    s1reg.b = 0;
  Int<64> s1reg_flatten = 0;
  s1reg_flatten.at<31, 0>() = Int<32>(s1reg.a);
  s1reg_flatten.at<63, 32>() = Int<32>(s1reg.b);
  resetvalue_s1reg__ = s1reg_flatten;
}
{
  bool s2valid;
    s2valid = false;
  Int<1> s2valid_flatten = 0;
  s2valid_flatten.at<0, 0>() = Int<1>(s2valid);
  resetvalue_s2valid__ = s2valid_flatten;
}
{
  S1S2RegData s2reg;
    s2reg.p0 = 0;
    s2reg.p1 = 0;
    s2reg.p2 = 0;
    s2reg.p3 = 0;
  Int<128> s2reg_flatten = 0;
  s2reg_flatten.at<31, 0>() = Int<32>(s2reg.p0);
  s2reg_flatten.at<63, 32>() = Int<32>(s2reg.p1);
  s2reg_flatten.at<95, 64>() = Int<32>(s2reg.p2);
  s2reg_flatten.at<127, 96>() = Int<32>(s2reg.p3);
  resetvalue_s2reg__ = s2reg_flatten;
}
{
  bool s3valid;
    s3valid = false;
  Int<1> s3valid_flatten = 0;
  s3valid_flatten.at<0, 0>() = Int<1>(s3valid);
  resetvalue_s3valid__ = s3valid_flatten;
}
{
  S2S3RegData s3reg;
    s3reg.low16 = 0;
    s3reg.mid = 0;
    s3reg.high_base = 0;
  Int<82> s3reg_flatten = 0;
  s3reg_flatten.at<15, 0>() = Int<16>(s3reg.low16);
  s3reg_flatten.at<49, 16>() = Int<34>(s3reg.mid);
  s3reg_flatten.at<81, 50>() = Int<32>(s3reg.high_base);
  resetvalue_s3reg__ = s3reg_flatten;
}

auto s0input_impl__ = [&](uint32_t a, uint32_t b) -> void {
    __vul_reg_setnext_s1reg<0>(S0S1RegData{a, b});
    __vul_reg_setnext_s1valid<0>(true);
};
auto tick0__ = [&]() {
    __vul_reg_setnext_s1valid<1>(false);
};
auto tick1__ = [&]() {
    __vul_reg_setnext_s2valid<0>(__vul_read_reg_s1valid());
    if (__vul_read_reg_s1valid()) {
        S0S1RegData s1 = __vul_read_reg_s1reg();
        Int<32> a = s1.a;
        Int<32> b = s1.b;
        Int<16> a_lo = a.at<15, 0>();
        Int<16> b_lo = b.at<15, 0>();
        Int<16> a_hi = a.at<31, 16>();
        Int<16> b_hi = b.at<31, 16>();
        S1S2RegData s2;
        s2.p0 = a_lo * b_lo;
        s2.p1 = a_lo * b_hi;
        s2.p2 = a_hi * b_lo;
        s2.p3 = a_hi * b_hi;
        __vul_reg_setnext_s2reg<0>(s2);
    };
};
auto tick2__ = [&]() {
    __vul_reg_setnext_s3valid<0>(__vul_read_reg_s2valid());
    if (__vul_read_reg_s2valid()) {
        S1S2RegData s2 = __vul_read_reg_s2reg();
        S2S3RegData s3;
        s3.low16 = s2.p0.at<15, 0>();
        s3.mid = s2.p0.at<31, 16>() + s2.p1 + s2.p2;
        s3.high_base = s2.p3;
        __vul_reg_setnext_s3reg<0>(s3);
    };
};
auto tick3__ = [&]() {
    if (__vul_read_reg_s3valid()) {
        S2S3RegData s3 = __vul_read_reg_s3reg();
        Int<33> high = s3.high_base + s3.mid.at<33, 16>();
        Int<64> y = Cat(high.at<31, 0>(), s3.mid.at<15, 0>(), s3.low16);
        __vul_req_call_s3output(y.to<uint64_t>());
    };
};

{
  uint32_t a = {};
  a = Int<32>(s0input_a__.at<31, 0>()).template to<typename std::remove_reference<decltype(a)>::type>();
  uint32_t b = {};
  b = Int<32>(s0input_b__.at<31, 0>()).template to<typename std::remove_reference<decltype(b)>::type>();
  if (s0input__vld__) {
    s0input_impl__(a, b);
  }
}
tick0__();
tick1__();
tick2__();
tick3__();

}
