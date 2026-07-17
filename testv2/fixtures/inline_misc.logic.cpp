#include <fixint.hpp>

Int<8> add_bias(Int<8> x) {
    return x + Int<8>(3);
}

Int<8> choose_path(bool sel, Int<8> a, Int<8> b) {
    if (sel) {
        return a + Int<8>(1);
    }
    return b ^ Int<8>(2);
}

Int<8> mix_helper(Int<8> x, Int<8> y) {
    Int<8> biased = add_bias(x);
    return biased ^ y;
}

void mutate_acc(Int<8>& acc, const Int<8>& delta, bool sel) {
    if (sel) {
        acc = acc + delta;
        return;
    }
    acc = acc ^ delta;
}

Int<8> overloaded_pick(Int<8> x) {
    return x + Int<8>(5);
}

Int<12> overloaded_pick(Int<12> x) {
    return x ^ Int<12>(0x2d);
}

Int<8> helper_with_local_lambda(Int<8> x, bool sel) {
    auto local = [sel](Int<8> value) -> Int<8> {
        if (sel) {
            return value + Int<8>(7);
        }
        return value ^ Int<8>(9);
    };
    return local(x);
}

Int<8> loop_calls_helper(Int<8> seed) {
    Int<8> acc = seed;
    for (int i = 0; i < 2; i = i + 1) {
        acc = add_bias(acc);
    }
    return acc;
}

void hls_main(Int<8> a,
              Int<8> b,
              Int<12> wide,
              bool sel,
              bool alt,
              Int<8>& helper_chain,
              Int<8>& ref_update,
              Int<8>& overload8,
              Int<12>& overload12,
              Int<8>& lambda_value,
              Int<8>& lambda_ref,
              Int<8>& helper_lambda,
              Int<8>& loop_inline,
              Int<8>& return_mix) {
    Int<8> chain = mix_helper(a, b);
    helper_chain = chain;

    Int<8> ref_acc = chain;
    mutate_acc(ref_acc, b, sel);
    ref_update = ref_acc;

    overload8 = overloaded_pick(a);
    overload12 = overloaded_pick(wide);

    auto value_lambda = [sel](Int<8> x, Int<8> y) -> Int<8> {
        Int<8> mixed = mix_helper(x, y);
        return choose_path(sel, mixed, y);
    };
    lambda_value = value_lambda(a, b);

    Int<8> touched = value_lambda(a, b);
    if (!alt) {
        touched = touched + Int<8>(1);
    }
    lambda_ref = touched;

    helper_lambda = helper_with_local_lambda(a, alt);
    loop_inline = loop_calls_helper(b);

    Int<8> helper_side = helper_with_local_lambda(chain, alt);
    Int<8> loop_side = loop_calls_helper(a);
    return_mix = choose_path(sel, helper_side, loop_side);
}
