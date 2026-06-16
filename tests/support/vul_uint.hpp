#pragma once

template<int N>
struct UInt;
template<int N>
struct Int;

struct VULBitRef {
    operator bool() const;
    VULBitRef& operator=(bool);
};

template<int N>
struct VULSliceRef {
    operator UInt<N>() const;
    operator Int<N>() const;
    template<int M>
    VULSliceRef& operator=(UInt<M>);
    template<int M>
    VULSliceRef& operator=(Int<M>);
    VULSliceRef& operator=(VULSliceRef<N>);
};

template<int N>
struct UInt {
    UInt();

    template<int M>
    UInt(UInt<M>);
    template<int M>
    UInt& operator=(UInt<M>);
    template<int M>
    UInt& operator=(Int<M>);
    template<int M>
    UInt& operator=(VULSliceRef<M>);
    UInt& operator=(bool);

    UInt(unsigned long long);
    operator int() const;

    VULBitRef operator()(int);
    bool operator()(int) const;

    template<int W>
    VULSliceRef<W> slice_ref(int, int);
    template<int W>
    UInt<W> range_at(int) const;
    bool bit_at(int) const;

    VULSliceRef<N> operator()(int, int);
    UInt<N> operator()(int, int) const;

    template<int K>
    UInt<N * K> repeat() const;
    template<int M>
    UInt<N + M> cat(UInt<M>) const;
    bool reduce_or() const;
    bool reduce_and() const;
    bool reduce_xor() const;
};

template<int N>
struct Int {
    Int();

    template<int M>
    Int(Int<M>);
    template<int M>
    Int(UInt<M>);
    template<int M>
    Int& operator=(Int<M>);
    template<int M>
    Int& operator=(UInt<M>);
    template<int M>
    Int& operator=(VULSliceRef<M>);
    Int& operator=(bool);

    Int(long long);
    operator int() const;

    VULBitRef operator()(int);
    bool operator()(int) const;

    template<int W>
    Int<W> range_at(int) const;
    bool bit_at(int) const;

    VULSliceRef<N> operator()(int, int);
    Int<N> operator()(int, int) const;

    template<int K>
    Int<N * K> repeat() const;
    template<int M>
    Int<N + M> cat(Int<M>) const;
    bool reduce_or() const;
    bool reduce_and() const;
    bool reduce_xor() const;
};

template<int A, int B>
UInt<(A > B ? A : B) + 1> operator+(UInt<A>, UInt<B>);

template<int A, int B>
Int<(A > B ? A : B) + 1> operator+(Int<A>, Int<B>);

template<int A, int B>
UInt<(A > B ? A : B)> operator-(UInt<A>, UInt<B>);

template<int A, int B>
Int<(A > B ? A : B)> operator-(Int<A>, Int<B>);

template<int A, int B>
UInt<A + B> operator*(UInt<A>, UInt<B>);

template<int A, int B>
Int<A + B> operator*(Int<A>, Int<B>);

template<int A, int B>
Int<A + B> Cat(Int<A>, Int<B>);

template<int A, int B, int C>
Int<A + B + C> Cat(Int<A>, Int<B>, Int<C>);

template<int To, int From>
UInt<To> zext(UInt<From> x) {
    return UInt<To>(x);
}

template<int To, int From>
UInt<To> trunc(UInt<From> x) {
    return UInt<To>(x);
}

template<int To, int From>
Int<To> zext(Int<From> x) {
    return Int<To>(x);
}

template<int To, int From>
Int<To> trunc(Int<From> x) {
    return Int<To>(x);
}
