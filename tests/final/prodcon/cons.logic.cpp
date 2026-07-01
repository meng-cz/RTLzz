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

struct __RegProxy_uint8_t__cycle {
const Int<8> &rdata;
bool &wen;
Int<8> &wdata;

__RegProxy_uint8_t__cycle(const Int<8> &rdata, bool &wen, Int<8> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator uint8_t() const {
  uint8_t value;
  value = rdata.at<7, 0>();
  return value;
}
uint8_t get() const {
  uint8_t value;
  value = rdata.at<7, 0>();
  return value;
}
template <uint32_t P = 0>
void setnext(const uint8_t &value) {
  static_assert(P < 1, "Port index out of range");
  Int<8> wdata_val;
  wdata_val.at<7, 0>() = value;
  wdata = wdata_val;
  wen = true;
}
};

struct __RegProxy_uint8_t__sum {
const Int<8> &rdata;
bool &wen;
Int<8> &wdata;

__RegProxy_uint8_t__sum(const Int<8> &rdata, bool &wen, Int<8> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator uint8_t() const {
  uint8_t value;
  value = rdata.at<7, 0>();
  return value;
}
uint8_t get() const {
  uint8_t value;
  value = rdata.at<7, 0>();
  return value;
}
template <uint32_t P = 0>
void setnext(const uint8_t &value) {
  static_assert(P < 1, "Port index out of range");
  Int<8> wdata_val;
  wdata_val.at<7, 0>() = value;
  wdata = wdata_val;
  wen = true;
}
};

struct __ReqHelper__output {
  bool & vld_ports;
  Int<8> & arg_s;

  template <uint32_t IDX = 0>
  void call(const uint8_t & s) {
    static_assert(IDX < 1, "Request port index out of range");
    vld_ports = true;
    arg_s.at<7, 0>() = s;
  }
};

void LogicSubModule_Consumer_top_cons(
  const Int<8> &rdata_cycle__,
  bool &wen_cycle__,
  Int<8> &wdata_cycle__,
  const Int<8> &rdata_sum__,
  bool &wen_sum__,
  Int<8> &wdata_sum__,
  bool &output__vld__,
  Int<8> &output_s__,
  const bool recv__vld__,
  bool & recv__rdy__,
  const Int<8> recv_d__
) {
__RegProxy_uint8_t__cycle cycle(rdata_cycle__, wen_cycle__, wdata_cycle__);
__RegProxy_uint8_t__sum sum(rdata_sum__, wen_sum__, wdata_sum__);

__ReqHelper__output __req_helper__output{
  output__vld__,
  output_s__,
};
#define output __req_helper__output.call

auto recv_impl__ = [&](const uint8_t & d) -> void {
    sum.setnext(sum + d);
    output(sum);
};
auto recv_cond__ = [&](const uint8_t & d) -> bool {
return ((cycle & 1) == 0);
};
auto tick0__ = [&]() {
    cycle.setnext(cycle + 1);
};

{
  uint8_t d;
  d = recv_d__.at<7, 0>();
  bool rdy = recv_cond__(d);
  if (rdy && recv__vld__) {
    recv_impl__(d);
  }
  recv__rdy__ = rdy;
}
tick0__();

#undef output
}

