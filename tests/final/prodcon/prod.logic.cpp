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

struct __RegProxy_uint8_t__count {
const Int<8> &rdata;
bool &wen;
Int<8> &wdata;

__RegProxy_uint8_t__count(const Int<8> &rdata, bool &wen, Int<8> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

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

struct __ReqHelper__send {
  bool & vld_ports;
  const bool & rdy_ports;
  Int<8> & arg_d;

  template <uint32_t IDX = 0>
  bool call(const uint8_t & d) {
    static_assert(IDX < 1, "Request port index out of range");
    vld_ports = true;
    arg_d.at<7, 0>() = d;
    return rdy_ports;
  }
};

void LogicSubModule_Producer_top_prod(
  const Int<8> &rdata_cycle__,
  bool &wen_cycle__,
  Int<8> &wdata_cycle__,
  const Int<8> &rdata_count__,
  bool &wen_count__,
  Int<8> &wdata_count__,
  bool &send__vld__,
  const bool send__rdy__,
  Int<8> &send_d__
) {
__RegProxy_uint8_t__cycle cycle(rdata_cycle__, wen_cycle__, wdata_cycle__);
__RegProxy_uint8_t__count count(rdata_count__, wen_count__, wdata_count__);

__ReqHelper__send __req_helper__send{
  send__vld__,
  send__rdy__,
  send_d__,
};
#define send __req_helper__send.call

auto tick0__ = [&]() {
    cycle.setnext(cycle + 1);
    if (send(cycle)) {
        count.setnext(count + 1);
    }
};

tick0__();

#undef send
}

