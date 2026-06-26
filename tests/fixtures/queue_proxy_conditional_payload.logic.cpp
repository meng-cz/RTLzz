#include <uint.hpp>

struct QueueProxy_q__ {
    bool& q__enqvalid__;
    Int<8>& q__enqdata__;

    QueueProxy_q__(bool& valid, Int<8>& data)
        : q__enqvalid__(valid), q__enqdata__(data) {}

    void enqnext(const Int<8>& value) {
        q__enqdata__.at<7, 0>() = value;
        q__enqvalid__ = true;
    }
};

void hls_main(bool fire,
              Int<8> data,
              bool& q__enqvalid__,
              Int<8>& q__enqdata__) {
    QueueProxy_q__ q(q__enqvalid__, q__enqdata__);
    if (fire) {
        q.enqnext(data);
    }
}
