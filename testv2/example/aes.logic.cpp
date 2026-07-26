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

using AESData = std::array<uint8_t, 16>;

using AESKey = std::array<uint8_t, 16>;


#pragma input_port rdata_d__
Int<128> rdata_d__;
#pragma output_port wen_d__
std::array<bool, 2> wen_d__;
#pragma output_port wdata_d__
std::array<Int<128>, 2> wdata_d__;
#pragma output_port holdnext_d__
bool holdnext_d__;
#pragma output_port resetnext_d__
bool resetnext_d__;
#pragma output_port resetvalue_d__
Int<128> resetvalue_d__;
#pragma input_port rdata_k__
Int<128> rdata_k__;
#pragma output_port wen_k__
std::array<bool, 2> wen_k__;
#pragma output_port wdata_k__
std::array<Int<128>, 2> wdata_k__;
#pragma output_port holdnext_k__
bool holdnext_k__;
#pragma output_port resetnext_k__
bool resetnext_k__;
#pragma output_port resetvalue_k__
Int<128> resetvalue_k__;
#pragma input_port rdata_state__
Int<32> rdata_state__;
#pragma output_port wen_state__
std::array<bool, 2> wen_state__;
#pragma output_port wdata_state__
std::array<Int<32>, 2> wdata_state__;
#pragma output_port holdnext_state__
bool holdnext_state__;
#pragma output_port resetnext_state__
bool resetnext_state__;
#pragma output_port resetvalue_state__
Int<32> resetvalue_state__;
#pragma output_port output__vld__
bool output__vld__;
#pragma output_port output_data__
Int<128> output_data__;
#pragma input_port input__vld__
bool input__vld__;
#pragma output_port input__rdy__
bool  input__rdy__;
#pragma input_port input_data__
Int<128> input_data__;
#pragma input_port input_key__
Int<128> input_key__;

uint32_t __vul_read_reg_state() {
  const Int<32> &__vul_rdata = rdata_state__;
  uint32_t value = 0;
  value = Int<32>(__vul_rdata.at<31, 0>()).template to<typename std::remove_reference<decltype(value)>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_state(uint32_t value) {
  Int<32> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<31, 0>() = Int<32>(value);
  wdata_state__[P] = __vul_reg_wdata;
  wen_state__[P] = true;
}
void __vul_reg_holdnext_state() {
  holdnext_state__ = true;
}
void __vul_reg_resetnext_state() {
  resetnext_state__ = true;
}

AESKey __vul_read_reg_k() {
  const Int<128> &__vul_rdata = rdata_k__;
  AESKey value = {};
  value[0] = Int<8>(__vul_rdata.at<7, 0>()).template to<typename std::remove_reference<decltype(value[0])>::type>();
  value[1] = Int<8>(__vul_rdata.at<15, 8>()).template to<typename std::remove_reference<decltype(value[1])>::type>();
  value[2] = Int<8>(__vul_rdata.at<23, 16>()).template to<typename std::remove_reference<decltype(value[2])>::type>();
  value[3] = Int<8>(__vul_rdata.at<31, 24>()).template to<typename std::remove_reference<decltype(value[3])>::type>();
  value[4] = Int<8>(__vul_rdata.at<39, 32>()).template to<typename std::remove_reference<decltype(value[4])>::type>();
  value[5] = Int<8>(__vul_rdata.at<47, 40>()).template to<typename std::remove_reference<decltype(value[5])>::type>();
  value[6] = Int<8>(__vul_rdata.at<55, 48>()).template to<typename std::remove_reference<decltype(value[6])>::type>();
  value[7] = Int<8>(__vul_rdata.at<63, 56>()).template to<typename std::remove_reference<decltype(value[7])>::type>();
  value[8] = Int<8>(__vul_rdata.at<71, 64>()).template to<typename std::remove_reference<decltype(value[8])>::type>();
  value[9] = Int<8>(__vul_rdata.at<79, 72>()).template to<typename std::remove_reference<decltype(value[9])>::type>();
  value[10] = Int<8>(__vul_rdata.at<87, 80>()).template to<typename std::remove_reference<decltype(value[10])>::type>();
  value[11] = Int<8>(__vul_rdata.at<95, 88>()).template to<typename std::remove_reference<decltype(value[11])>::type>();
  value[12] = Int<8>(__vul_rdata.at<103, 96>()).template to<typename std::remove_reference<decltype(value[12])>::type>();
  value[13] = Int<8>(__vul_rdata.at<111, 104>()).template to<typename std::remove_reference<decltype(value[13])>::type>();
  value[14] = Int<8>(__vul_rdata.at<119, 112>()).template to<typename std::remove_reference<decltype(value[14])>::type>();
  value[15] = Int<8>(__vul_rdata.at<127, 120>()).template to<typename std::remove_reference<decltype(value[15])>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_k(AESKey value) {
  Int<128> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<7, 0>() = Int<8>(value[0]);
  __vul_reg_wdata.at<15, 8>() = Int<8>(value[1]);
  __vul_reg_wdata.at<23, 16>() = Int<8>(value[2]);
  __vul_reg_wdata.at<31, 24>() = Int<8>(value[3]);
  __vul_reg_wdata.at<39, 32>() = Int<8>(value[4]);
  __vul_reg_wdata.at<47, 40>() = Int<8>(value[5]);
  __vul_reg_wdata.at<55, 48>() = Int<8>(value[6]);
  __vul_reg_wdata.at<63, 56>() = Int<8>(value[7]);
  __vul_reg_wdata.at<71, 64>() = Int<8>(value[8]);
  __vul_reg_wdata.at<79, 72>() = Int<8>(value[9]);
  __vul_reg_wdata.at<87, 80>() = Int<8>(value[10]);
  __vul_reg_wdata.at<95, 88>() = Int<8>(value[11]);
  __vul_reg_wdata.at<103, 96>() = Int<8>(value[12]);
  __vul_reg_wdata.at<111, 104>() = Int<8>(value[13]);
  __vul_reg_wdata.at<119, 112>() = Int<8>(value[14]);
  __vul_reg_wdata.at<127, 120>() = Int<8>(value[15]);
  wdata_k__[P] = __vul_reg_wdata;
  wen_k__[P] = true;
}
void __vul_reg_holdnext_k() {
  holdnext_k__ = true;
}
void __vul_reg_resetnext_k() {
  resetnext_k__ = true;
}

AESData __vul_read_reg_d() {
  const Int<128> &__vul_rdata = rdata_d__;
  AESData value = {};
  value[0] = Int<8>(__vul_rdata.at<7, 0>()).template to<typename std::remove_reference<decltype(value[0])>::type>();
  value[1] = Int<8>(__vul_rdata.at<15, 8>()).template to<typename std::remove_reference<decltype(value[1])>::type>();
  value[2] = Int<8>(__vul_rdata.at<23, 16>()).template to<typename std::remove_reference<decltype(value[2])>::type>();
  value[3] = Int<8>(__vul_rdata.at<31, 24>()).template to<typename std::remove_reference<decltype(value[3])>::type>();
  value[4] = Int<8>(__vul_rdata.at<39, 32>()).template to<typename std::remove_reference<decltype(value[4])>::type>();
  value[5] = Int<8>(__vul_rdata.at<47, 40>()).template to<typename std::remove_reference<decltype(value[5])>::type>();
  value[6] = Int<8>(__vul_rdata.at<55, 48>()).template to<typename std::remove_reference<decltype(value[6])>::type>();
  value[7] = Int<8>(__vul_rdata.at<63, 56>()).template to<typename std::remove_reference<decltype(value[7])>::type>();
  value[8] = Int<8>(__vul_rdata.at<71, 64>()).template to<typename std::remove_reference<decltype(value[8])>::type>();
  value[9] = Int<8>(__vul_rdata.at<79, 72>()).template to<typename std::remove_reference<decltype(value[9])>::type>();
  value[10] = Int<8>(__vul_rdata.at<87, 80>()).template to<typename std::remove_reference<decltype(value[10])>::type>();
  value[11] = Int<8>(__vul_rdata.at<95, 88>()).template to<typename std::remove_reference<decltype(value[11])>::type>();
  value[12] = Int<8>(__vul_rdata.at<103, 96>()).template to<typename std::remove_reference<decltype(value[12])>::type>();
  value[13] = Int<8>(__vul_rdata.at<111, 104>()).template to<typename std::remove_reference<decltype(value[13])>::type>();
  value[14] = Int<8>(__vul_rdata.at<119, 112>()).template to<typename std::remove_reference<decltype(value[14])>::type>();
  value[15] = Int<8>(__vul_rdata.at<127, 120>()).template to<typename std::remove_reference<decltype(value[15])>::type>();
  return value;
}
template <uint32_t P = 0>
void __vul_reg_setnext_d(AESData value) {
  Int<128> __vul_reg_wdata = 0;
  __vul_reg_wdata.at<7, 0>() = Int<8>(value[0]);
  __vul_reg_wdata.at<15, 8>() = Int<8>(value[1]);
  __vul_reg_wdata.at<23, 16>() = Int<8>(value[2]);
  __vul_reg_wdata.at<31, 24>() = Int<8>(value[3]);
  __vul_reg_wdata.at<39, 32>() = Int<8>(value[4]);
  __vul_reg_wdata.at<47, 40>() = Int<8>(value[5]);
  __vul_reg_wdata.at<55, 48>() = Int<8>(value[6]);
  __vul_reg_wdata.at<63, 56>() = Int<8>(value[7]);
  __vul_reg_wdata.at<71, 64>() = Int<8>(value[8]);
  __vul_reg_wdata.at<79, 72>() = Int<8>(value[9]);
  __vul_reg_wdata.at<87, 80>() = Int<8>(value[10]);
  __vul_reg_wdata.at<95, 88>() = Int<8>(value[11]);
  __vul_reg_wdata.at<103, 96>() = Int<8>(value[12]);
  __vul_reg_wdata.at<111, 104>() = Int<8>(value[13]);
  __vul_reg_wdata.at<119, 112>() = Int<8>(value[14]);
  __vul_reg_wdata.at<127, 120>() = Int<8>(value[15]);
  wdata_d__[P] = __vul_reg_wdata;
  wen_d__[P] = true;
}
void __vul_reg_holdnext_d() {
  holdnext_d__ = true;
}
void __vul_reg_resetnext_d() {
  resetnext_d__ = true;
}

void __vul_req_call_output(AESData data) {
  output__vld__ = true;
  AESData __vul_req_arg_data = (data);
  output_data__.at<7, 0>() = Int<8>(data[0]);
  output_data__.at<15, 8>() = Int<8>(data[1]);
  output_data__.at<23, 16>() = Int<8>(data[2]);
  output_data__.at<31, 24>() = Int<8>(data[3]);
  output_data__.at<39, 32>() = Int<8>(data[4]);
  output_data__.at<47, 40>() = Int<8>(data[5]);
  output_data__.at<55, 48>() = Int<8>(data[6]);
  output_data__.at<63, 56>() = Int<8>(data[7]);
  output_data__.at<71, 64>() = Int<8>(data[8]);
  output_data__.at<79, 72>() = Int<8>(data[9]);
  output_data__.at<87, 80>() = Int<8>(data[10]);
  output_data__.at<95, 88>() = Int<8>(data[11]);
  output_data__.at<103, 96>() = Int<8>(data[12]);
  output_data__.at<111, 104>() = Int<8>(data[13]);
  output_data__.at<119, 112>() = Int<8>(data[14]);
  output_data__.at<127, 120>() = Int<8>(data[15]);
}

void LogicSubModule_AES1_top() {
holdnext_d__ = false;
resetnext_d__ = false;
for (uint32_t __vul_p = 0; __vul_p < 2; ++__vul_p) {
  wen_d__[__vul_p] = false;
  wdata_d__[__vul_p] = 0;
}
holdnext_k__ = false;
resetnext_k__ = false;
for (uint32_t __vul_p = 0; __vul_p < 2; ++__vul_p) {
  wen_k__[__vul_p] = false;
  wdata_k__[__vul_p] = 0;
}
holdnext_state__ = false;
resetnext_state__ = false;
for (uint32_t __vul_p = 0; __vul_p < 2; ++__vul_p) {
  wen_state__[__vul_p] = false;
  wdata_state__[__vul_p] = 0;
}
output__vld__ = false;
output_data__ = 0;

{
  AESData d;
    for (uint32_t i = 0; i < 16; i++) {
        d[i] = 0;
    }
  Int<128> d_flatten = 0;
  d_flatten.at<7, 0>() = Int<8>(d[0]);
  d_flatten.at<15, 8>() = Int<8>(d[1]);
  d_flatten.at<23, 16>() = Int<8>(d[2]);
  d_flatten.at<31, 24>() = Int<8>(d[3]);
  d_flatten.at<39, 32>() = Int<8>(d[4]);
  d_flatten.at<47, 40>() = Int<8>(d[5]);
  d_flatten.at<55, 48>() = Int<8>(d[6]);
  d_flatten.at<63, 56>() = Int<8>(d[7]);
  d_flatten.at<71, 64>() = Int<8>(d[8]);
  d_flatten.at<79, 72>() = Int<8>(d[9]);
  d_flatten.at<87, 80>() = Int<8>(d[10]);
  d_flatten.at<95, 88>() = Int<8>(d[11]);
  d_flatten.at<103, 96>() = Int<8>(d[12]);
  d_flatten.at<111, 104>() = Int<8>(d[13]);
  d_flatten.at<119, 112>() = Int<8>(d[14]);
  d_flatten.at<127, 120>() = Int<8>(d[15]);
  resetvalue_d__ = d_flatten;
}
{
  AESKey k;
    for (uint32_t i = 0; i < 16; i++) {
        k[i] = 0;
    }
  Int<128> k_flatten = 0;
  k_flatten.at<7, 0>() = Int<8>(k[0]);
  k_flatten.at<15, 8>() = Int<8>(k[1]);
  k_flatten.at<23, 16>() = Int<8>(k[2]);
  k_flatten.at<31, 24>() = Int<8>(k[3]);
  k_flatten.at<39, 32>() = Int<8>(k[4]);
  k_flatten.at<47, 40>() = Int<8>(k[5]);
  k_flatten.at<55, 48>() = Int<8>(k[6]);
  k_flatten.at<63, 56>() = Int<8>(k[7]);
  k_flatten.at<71, 64>() = Int<8>(k[8]);
  k_flatten.at<79, 72>() = Int<8>(k[9]);
  k_flatten.at<87, 80>() = Int<8>(k[10]);
  k_flatten.at<95, 88>() = Int<8>(k[11]);
  k_flatten.at<103, 96>() = Int<8>(k[12]);
  k_flatten.at<111, 104>() = Int<8>(k[13]);
  k_flatten.at<119, 112>() = Int<8>(k[14]);
  k_flatten.at<127, 120>() = Int<8>(k[15]);
  resetvalue_k__ = k_flatten;
}
{
  uint32_t state;
    state = 0;
  Int<32> state_flatten = 0;
  state_flatten.at<31, 0>() = Int<32>(state);
  resetvalue_state__ = state_flatten;
}

auto input_impl__ = [&](AESData data, AESKey key) -> void {
    __vul_reg_setnext_k<0>(key);
    AESData indata;
    for (uint32_t i = 0; i < 16; i++) {
        indata[i] = data[i] ^ key[i];
    }
    __vul_reg_setnext_d<0>(indata);
    __vul_reg_setnext_state<0>(1);
};
auto input_cond__ = [&](AESData data, AESKey key) -> bool {
return ((__vul_read_reg_state() == 0 || __vul_read_reg_state() >= 10));
};
auto tick0__ = [&]() {
    constexpr uint8_t SBOX[256] = {
        0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
        0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
        0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
        0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
        0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
        0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
        0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
        0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
        0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
        0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
        0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
        0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
        0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
        0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
        0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
        0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};
    constexpr uint8_t RC[] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    auto shift_rows = [](AESData& data) {
        uint8_t temp = 0;
        temp        = data[1];
        data[1]    = data[5];
        data[5]    = data[9];
        data[9]    = data[13];
        data[13]   = temp;
        temp        = data[2];
        data[2]    = data[10];
        data[10]   = temp;
        temp        = data[6];
        data[6]    = data[14];
        data[14]   = temp;
        temp        = data[3];
        data[3]    = data[7];
        data[7]    = data[11];
        data[11]   = data[15];
        data[15]   = temp;
    };
    auto mul2 = [](uint8_t x) {
        uint8_t m = -(x >> 7);
        return (x << 1) ^ (0x1b & m);
    };
    if (__vul_read_reg_state() == 0 || __vul_read_reg_state() > 10) {
        return;
    }
    AESData data = __vul_read_reg_d();
    AESKey round_key = __vul_read_reg_k();
    uint8_t t0 = SBOX[round_key[13]] ^ RC[__vul_read_reg_state()-1];
    uint8_t t1 = SBOX[round_key[14]];
    uint8_t t2 = SBOX[round_key[15]];
    uint8_t t3 = SBOX[round_key[12]];
    round_key[0] ^= t0;
    round_key[1] ^= t1;
    round_key[2] ^= t2;
    round_key[3] ^= t3;
    for (int i = 4; i < 16; i++) {
        round_key[i] ^= round_key[i-4];
    }
    AESData tmp;
    for (uint32_t i = 0; i < 16; ++i) {
        tmp[i] = SBOX[data[i]];
    }
    shift_rows(tmp);
    if (__vul_read_reg_state() != 10) {
        for (uint32_t i = 0; i < 16; i+=4) {
            uint8_t t = tmp[i] ^ tmp[i+1] ^ tmp[i+2] ^ tmp[i+3];
            data[i] = mul2(tmp[i]  ^ tmp[i+1]) ^ tmp[i]   ^ t;
            data[i+1] = mul2(tmp[i+1] ^ tmp[i+2]) ^ tmp[i+1] ^ t;
            data[i+2] = mul2(tmp[i+2] ^ tmp[i+3]) ^ tmp[i+2] ^ t;
            data[i+3] = mul2(tmp[i+3] ^ tmp[i]  ) ^ tmp[i+3] ^ t;
        }
    }
    for (uint32_t i = 0; i < 16; ++i) {
        data[i] ^= round_key[i];
    }
    if (__vul_read_reg_state() == 10) {
        __vul_req_call_output(data);
        __vul_reg_setnext_state<1>(0);
    } else {
        __vul_reg_setnext_d<1>(data);
        __vul_reg_setnext_k<1>(round_key);
        __vul_reg_setnext_state<1>(__vul_read_reg_state() + 1);
    }
};

{
  AESData data = {};
  data[0] = Int<8>(input_data__.at<7, 0>()).template to<typename std::remove_reference<decltype(data[0])>::type>();
  data[1] = Int<8>(input_data__.at<15, 8>()).template to<typename std::remove_reference<decltype(data[1])>::type>();
  data[2] = Int<8>(input_data__.at<23, 16>()).template to<typename std::remove_reference<decltype(data[2])>::type>();
  data[3] = Int<8>(input_data__.at<31, 24>()).template to<typename std::remove_reference<decltype(data[3])>::type>();
  data[4] = Int<8>(input_data__.at<39, 32>()).template to<typename std::remove_reference<decltype(data[4])>::type>();
  data[5] = Int<8>(input_data__.at<47, 40>()).template to<typename std::remove_reference<decltype(data[5])>::type>();
  data[6] = Int<8>(input_data__.at<55, 48>()).template to<typename std::remove_reference<decltype(data[6])>::type>();
  data[7] = Int<8>(input_data__.at<63, 56>()).template to<typename std::remove_reference<decltype(data[7])>::type>();
  data[8] = Int<8>(input_data__.at<71, 64>()).template to<typename std::remove_reference<decltype(data[8])>::type>();
  data[9] = Int<8>(input_data__.at<79, 72>()).template to<typename std::remove_reference<decltype(data[9])>::type>();
  data[10] = Int<8>(input_data__.at<87, 80>()).template to<typename std::remove_reference<decltype(data[10])>::type>();
  data[11] = Int<8>(input_data__.at<95, 88>()).template to<typename std::remove_reference<decltype(data[11])>::type>();
  data[12] = Int<8>(input_data__.at<103, 96>()).template to<typename std::remove_reference<decltype(data[12])>::type>();
  data[13] = Int<8>(input_data__.at<111, 104>()).template to<typename std::remove_reference<decltype(data[13])>::type>();
  data[14] = Int<8>(input_data__.at<119, 112>()).template to<typename std::remove_reference<decltype(data[14])>::type>();
  data[15] = Int<8>(input_data__.at<127, 120>()).template to<typename std::remove_reference<decltype(data[15])>::type>();
  AESKey key = {};
  key[0] = Int<8>(input_key__.at<7, 0>()).template to<typename std::remove_reference<decltype(key[0])>::type>();
  key[1] = Int<8>(input_key__.at<15, 8>()).template to<typename std::remove_reference<decltype(key[1])>::type>();
  key[2] = Int<8>(input_key__.at<23, 16>()).template to<typename std::remove_reference<decltype(key[2])>::type>();
  key[3] = Int<8>(input_key__.at<31, 24>()).template to<typename std::remove_reference<decltype(key[3])>::type>();
  key[4] = Int<8>(input_key__.at<39, 32>()).template to<typename std::remove_reference<decltype(key[4])>::type>();
  key[5] = Int<8>(input_key__.at<47, 40>()).template to<typename std::remove_reference<decltype(key[5])>::type>();
  key[6] = Int<8>(input_key__.at<55, 48>()).template to<typename std::remove_reference<decltype(key[6])>::type>();
  key[7] = Int<8>(input_key__.at<63, 56>()).template to<typename std::remove_reference<decltype(key[7])>::type>();
  key[8] = Int<8>(input_key__.at<71, 64>()).template to<typename std::remove_reference<decltype(key[8])>::type>();
  key[9] = Int<8>(input_key__.at<79, 72>()).template to<typename std::remove_reference<decltype(key[9])>::type>();
  key[10] = Int<8>(input_key__.at<87, 80>()).template to<typename std::remove_reference<decltype(key[10])>::type>();
  key[11] = Int<8>(input_key__.at<95, 88>()).template to<typename std::remove_reference<decltype(key[11])>::type>();
  key[12] = Int<8>(input_key__.at<103, 96>()).template to<typename std::remove_reference<decltype(key[12])>::type>();
  key[13] = Int<8>(input_key__.at<111, 104>()).template to<typename std::remove_reference<decltype(key[13])>::type>();
  key[14] = Int<8>(input_key__.at<119, 112>()).template to<typename std::remove_reference<decltype(key[14])>::type>();
  key[15] = Int<8>(input_key__.at<127, 120>()).template to<typename std::remove_reference<decltype(key[15])>::type>();
  bool rdy = input_cond__(data, key);
  if (rdy && input__vld__) {
    input_impl__(data, key);
  }
  input__rdy__ = rdy;
}
tick0__();

}
