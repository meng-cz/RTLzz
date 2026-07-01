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

constexpr int64_t ADDR_WIDTH = 32;
constexpr int64_t DATA_WIDTH = 64;
constexpr int64_t INDEX_SIZE = 1024;
constexpr int64_t INDEX_WIDTH = 10;
constexpr int64_t TAG_WIDTH = 22;

struct ReadStageReg {
    Int<32> addr;
    bool valid;
};

struct TagEntry {
    Int<22> tag;
    bool valid;
};

struct __RegProxy_ReadStageReg__read_stage {
const Int<33> &rdata;
bool &wen;
Int<33> &wdata;

__RegProxy_ReadStageReg__read_stage(const Int<33> &rdata, bool &wen, Int<33> &wdata) : rdata(rdata), wen(wen), wdata(wdata) {}

operator ReadStageReg() const {
  ReadStageReg value;
  value.addr = rdata.at<31, 0>();
  value.valid = rdata.at<32, 32>();
  return value;
}
ReadStageReg get() const {
  ReadStageReg value;
  value.addr = rdata.at<31, 0>();
  value.valid = rdata.at<32, 32>();
  return value;
}
template <uint32_t P = 0>
void setnext(const ReadStageReg &value) {
  static_assert(P < 1, "Port index out of range");
  Int<33> wdata_val;
  wdata_val.at<31, 0>() = value.addr;
  wdata_val.at<32, 32>() = value.valid;
  wdata = wdata_val;
  wen = true;
}
};

struct __ReqHelper__readresp_s1 {
  bool & vld_ports;
  Int<1> & arg_hit;
  Int<64> & arg_data;

  template <uint32_t IDX = 0>
  void call(const bool & hit, const Int<64> & data) {
    static_assert(IDX < 1, "Request port index out of range");
    vld_ports = true;
    arg_hit.at<0, 0>() = hit;
    arg_data.at<63, 0>() = data;
  }
};
struct __BRAMProxy__tag_array {
  using DataType = TagEntry;
  using AddrType = Int<10>;
  std::array<bool, 1> &bram_tag_array__s1_readreq;
  std::array<Int<10>, 1> &bram_tag_array__s1_readaddr;
  const std::array<Int<23>, 1> bram_tag_array__s2_readdata;
  std::array<bool, 1> &bram_tag_array__s1_write;
  std::array<Int<10>, 1> &bram_tag_array__s1_writeaddr;
  std::array<Int<23>, 1> &bram_tag_array__s1_writedata;
  mutable std::array<DataType, 1> readdata_buf;

  __BRAMProxy__tag_array(std::array<bool, 1> &bram_tag_array__s1_readreq, std::array<Int<10>, 1> &bram_tag_array__s1_readaddr, const std::array<Int<23>, 1> bram_tag_array__s2_readdata, std::array<bool, 1> &bram_tag_array__s1_write, std::array<Int<10>, 1> &bram_tag_array__s1_writeaddr, std::array<Int<23>, 1> &bram_tag_array__s1_writedata) : bram_tag_array__s1_readreq(bram_tag_array__s1_readreq), bram_tag_array__s1_readaddr(bram_tag_array__s1_readaddr), bram_tag_array__s2_readdata(bram_tag_array__s2_readdata), bram_tag_array__s1_write(bram_tag_array__s1_write), bram_tag_array__s1_writeaddr(bram_tag_array__s1_writeaddr), bram_tag_array__s1_writedata(bram_tag_array__s1_writedata) {}

  template <uint32_t PortIndex>
  void readreq(const AddrType &addr) {
    static_assert(PortIndex < 1, "Read port index out of range");
    bram_tag_array__s1_readreq[PortIndex] = true;
    bram_tag_array__s1_readaddr[PortIndex] = addr;
  }

  template <uint32_t PortIndex>
  const DataType& readdata() const {
    static_assert(PortIndex < 1, "Read port index out of range");
    DataType value;
    value.tag = bram_tag_array__s2_readdata[PortIndex].at<21, 0>();
    value.valid = bram_tag_array__s2_readdata[PortIndex].at<22, 22>();
    readdata_buf[PortIndex] = value;
    return readdata_buf[PortIndex];
  }

  template <uint32_t PortIndex>
  void write(const AddrType &addr, const DataType &data) {
    static_assert(PortIndex < 1, "Write port index out of range");
    bram_tag_array__s1_write[PortIndex] = true;
    bram_tag_array__s1_writeaddr[PortIndex] = addr;
    Int<23> packed;
    packed.at<21, 0>() = data.tag;
    packed.at<22, 22>() = data.valid;
    bram_tag_array__s1_writedata[PortIndex] = packed;
  }
};

struct __BRAMProxy__data_array {
  using DataType = Int<64>;
  using AddrType = Int<10>;
  std::array<bool, 1> &bram_data_array__s1_readreq;
  std::array<Int<10>, 1> &bram_data_array__s1_readaddr;
  const std::array<Int<64>, 1> bram_data_array__s2_readdata;
  std::array<bool, 1> &bram_data_array__s1_write;
  std::array<Int<10>, 1> &bram_data_array__s1_writeaddr;
  std::array<Int<64>, 1> &bram_data_array__s1_writedata;
  mutable std::array<DataType, 1> readdata_buf;

  __BRAMProxy__data_array(std::array<bool, 1> &bram_data_array__s1_readreq, std::array<Int<10>, 1> &bram_data_array__s1_readaddr, const std::array<Int<64>, 1> bram_data_array__s2_readdata, std::array<bool, 1> &bram_data_array__s1_write, std::array<Int<10>, 1> &bram_data_array__s1_writeaddr, std::array<Int<64>, 1> &bram_data_array__s1_writedata) : bram_data_array__s1_readreq(bram_data_array__s1_readreq), bram_data_array__s1_readaddr(bram_data_array__s1_readaddr), bram_data_array__s2_readdata(bram_data_array__s2_readdata), bram_data_array__s1_write(bram_data_array__s1_write), bram_data_array__s1_writeaddr(bram_data_array__s1_writeaddr), bram_data_array__s1_writedata(bram_data_array__s1_writedata) {}

  template <uint32_t PortIndex>
  void readreq(const AddrType &addr) {
    static_assert(PortIndex < 1, "Read port index out of range");
    bram_data_array__s1_readreq[PortIndex] = true;
    bram_data_array__s1_readaddr[PortIndex] = addr;
  }

  template <uint32_t PortIndex>
  const DataType& readdata() const {
    static_assert(PortIndex < 1, "Read port index out of range");
    DataType value;
    value = bram_data_array__s2_readdata[PortIndex].at<63, 0>();
    readdata_buf[PortIndex] = value;
    return readdata_buf[PortIndex];
  }

  template <uint32_t PortIndex>
  void write(const AddrType &addr, const DataType &data) {
    static_assert(PortIndex < 1, "Write port index out of range");
    bram_data_array__s1_write[PortIndex] = true;
    bram_data_array__s1_writeaddr[PortIndex] = addr;
    Int<64> packed;
    packed.at<63, 0>() = data;
    bram_data_array__s1_writedata[PortIndex] = packed;
  }
};


void LogicSubModule_SimpleCache_top(
  const Int<33> &rdata_read_stage__,
  bool &wen_read_stage__,
  Int<33> &wdata_read_stage__,
  bool &readresp_s1__vld__,
  Int<1> &readresp_s1_hit__,
  Int<64> &readresp_s1_data__,
  const bool refill_s0__vld__,
  const Int<32> refill_s0_addr__,
  const Int<64> refill_s0_data__,
  const bool read_s0__vld__,
  const Int<32> read_s0_addr__,
  std::array<bool, 1> &bram_tag_array__s1_readreq,
  std::array<Int<10>, 1> &bram_tag_array__s1_readaddr,
  const std::array<Int<23>, 1> bram_tag_array__s2_readdata,
  std::array<bool, 1> &bram_tag_array__s1_write,
  std::array<Int<10>, 1> &bram_tag_array__s1_writeaddr,
  std::array<Int<23>, 1> &bram_tag_array__s1_writedata,
  std::array<bool, 1> &bram_data_array__s1_readreq,
  std::array<Int<10>, 1> &bram_data_array__s1_readaddr,
  const std::array<Int<64>, 1> bram_data_array__s2_readdata,
  std::array<bool, 1> &bram_data_array__s1_write,
  std::array<Int<10>, 1> &bram_data_array__s1_writeaddr,
  std::array<Int<64>, 1> &bram_data_array__s1_writedata
) {
bool read_inputed;
{
    read_inputed = false;
}
__RegProxy_ReadStageReg__read_stage read_stage(rdata_read_stage__, wen_read_stage__, wdata_read_stage__);
bram_tag_array__s1_readreq[0] = false;
bram_tag_array__s1_readaddr[0] = 0;
bram_tag_array__s1_write[0] = false;
bram_tag_array__s1_writeaddr[0] = 0;
bram_tag_array__s1_writedata[0] = 0;
__BRAMProxy__tag_array tag_array(bram_tag_array__s1_readreq, bram_tag_array__s1_readaddr, bram_tag_array__s2_readdata, bram_tag_array__s1_write, bram_tag_array__s1_writeaddr, bram_tag_array__s1_writedata);
bram_data_array__s1_readreq[0] = false;
bram_data_array__s1_readaddr[0] = 0;
bram_data_array__s1_write[0] = false;
bram_data_array__s1_writeaddr[0] = 0;
bram_data_array__s1_writedata[0] = 0;
__BRAMProxy__data_array data_array(bram_data_array__s1_readreq, bram_data_array__s1_readaddr, bram_data_array__s2_readdata, bram_data_array__s1_write, bram_data_array__s1_writeaddr, bram_data_array__s1_writedata);

__ReqHelper__readresp_s1 __req_helper__readresp_s1{
  readresp_s1__vld__,
  readresp_s1_hit__,
  readresp_s1_data__,
};
#define readresp_s1 __req_helper__readresp_s1.call

auto refill_s0_impl__ = [&](const Int<32> & addr, const Int<64> & data) -> void {
    Int<INDEX_WIDTH> index = addr.at<INDEX_WIDTH - 1, 0>();
    Int<TAG_WIDTH> tag = addr.at<ADDR_WIDTH - 1, INDEX_WIDTH>();
    TagEntry tag_entry;
    tag_entry.tag = tag;
    tag_entry.valid = true;
    tag_array.write<0>(index, tag_entry);
    data_array.write<0>(index, data);
};
auto read_s0_impl__ = [&](const Int<32> & addr) -> void {
    ReadStageReg s0;
    s0.addr = addr;
    s0.valid = true;
    read_stage.setnext(s0);
    Int<INDEX_WIDTH> index = addr.at<INDEX_WIDTH - 1, 0>();
    tag_array.readreq<0>(index);
    data_array.readreq<0>(index);
    read_inputed = true;
};
auto tick0__ = [&]() {
    bool hit = false;
    Int<DATA_WIDTH> read_data;
    if (read_stage.get().valid) {
        Int<TAG_WIDTH> tag = read_stage.get().addr.at<ADDR_WIDTH - 1, INDEX_WIDTH>();
        TagEntry tag_entry = tag_array.readdata<0>();
        Int<TAG_WIDTH> read_tag = tag_entry.tag;
        bool valid = tag_entry.valid;
        if (valid && read_tag == tag) {
            hit = true;
            read_data = data_array.readdata<0>();
        }
        readresp_s1(hit, read_data);
    }
    if (!read_inputed) {
        ReadStageReg s0;
        s0.valid = false;
        read_stage.setnext(s0);
    }
};

{
  Int<32> addr;
  addr = refill_s0_addr__.at<31, 0>();
  Int<64> data;
  data = refill_s0_data__.at<63, 0>();
  if (refill_s0__vld__) {
    refill_s0_impl__(addr, data);
  }
}
{
  Int<32> addr;
  addr = read_s0_addr__.at<31, 0>();
  if (read_s0__vld__) {
    read_s0_impl__(addr);
  }
}
tick0__();

#undef readresp_s1
}

