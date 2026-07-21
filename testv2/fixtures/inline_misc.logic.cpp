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

#pragma input_port a
Int<8> a;
#pragma input_port b
Int<8> b;
#pragma input_port wide
Int<12> wide;
#pragma input_port sel
bool sel;
#pragma input_port alt
bool alt;
#pragma output_port helper_chain
Int<8> helper_chain;
#pragma output_port ref_update
Int<8> ref_update;
#pragma output_port overload8
Int<8> overload8;
#pragma output_port overload12
Int<12> overload12;
#pragma output_port lambda_value
Int<8> lambda_value;
#pragma output_port lambda_ref
Int<8> lambda_ref;
#pragma output_port lambda_nested_value
Int<8> lambda_nested_value;
#pragma output_port lambda_nested_ref
Int<8> lambda_nested_ref;
#pragma output_port lambda_ref_mutation
Int<8> lambda_ref_mutation;
#pragma output_port lambda_calls_lambda_value
Int<8> lambda_calls_lambda_value;
#pragma output_port lambda_calls_lambda_ref
Int<8> lambda_calls_lambda_ref;
#pragma output_port lambda_template_0
Int<8> lambda_template_0;
#pragma output_port lambda_template_1
Int<8> lambda_template_1;
#pragma output_port helper_lambda
Int<8> helper_lambda;
#pragma output_port loop_inline
Int<8> loop_inline;
#pragma output_port return_mix
Int<8> return_mix;
#pragma output_port helper_global_read
Int<8> helper_global_read;
#pragma output_port helper_global_chain
Int<8> helper_global_chain;
#pragma output_port helper_global_write
Int<8> helper_global_write;
#pragma output_port helper_global_shadow
Int<8> helper_global_shadow;
#pragma output_port helper_template_global_0
Int<8> helper_template_global_0;
#pragma output_port helper_template_global_1
Int<8> helper_template_global_1;
#pragma output_port helper_default_array
Int<8> helper_default_array;
#pragma output_port helper_complex_lambda_arg
Int<8> helper_complex_lambda_arg;

Int<8> read_global_input() {
    return a + Int<8>(11);
}

Int<8> transitive_global_input() {
    return read_global_input() ^ b;
}

void write_global_output(Int<8> value) {
    helper_global_write = value + a;
}

Int<8> shadow_global_name(Int<8> a) {
    return a ^ ::a;
}

template <uint32_t P = 0>
void template_write_global(Int<8> value) {
    if constexpr (P == 0) {
        helper_template_global_0 = value;
    } else {
        helper_template_global_1 = value;
    }
}

Int<8> explicitly_initialized_array(Int<8> value) {
    std::array<Int<8>, 2> local = {};
    local[0] = value;
    local[1] = value + Int<8>(1);
    return local[0] ^ local[1];
}

void hls_main() {
    Int<8> chain = mix_helper(a, b);
    helper_chain = chain;

    Int<8> ref_acc = chain;
    mutate_acc(ref_acc, b, sel);
    ref_update = ref_acc;

    overload8 = overloaded_pick(a);
    overload12 = overloaded_pick(wide);

    auto value_lambda = [=](Int<8> x, Int<8> y) -> Int<8> {
        Int<8> mixed = mix_helper(x, y);
        return choose_path(sel, mixed, y);
    };
    lambda_value = value_lambda(a, b);

    Int<8> touched = value_lambda(a, b);
    if (!alt) {
        touched = touched + Int<8>(1);
    }
    lambda_ref = touched;

    auto nested_by_value = [=](Int<8> x) -> Int<8> {
        auto inner = [=](Int<8> y) -> Int<8> {
            return choose_path(sel, y, b);
        };
        return inner(x + Int<8>(2));
    };
    lambda_nested_value = nested_by_value(a);

    auto nested_by_ref = [&](Int<8> x) -> Int<8> {
        Int<8> local = x ^ Int<8>(4);
        auto inner = [&](Int<8> y) -> Int<8> {
            return choose_path(alt, local + y, b);
        };
        return inner(a);
    };
    lambda_nested_ref = nested_by_ref(chain);

    Int<8> mutable_main = a;
    auto mutate_main_and_output = [&](Int<8> delta) -> Int<8> {
        mutable_main = mutable_main + delta;
        lambda_ref_mutation = mutable_main ^ b;
        return lambda_ref_mutation;
    };
    Int<8> mutation_result = mutate_main_and_output(Int<8>(3));
    lambda_ref_mutation = mutation_result + mutable_main;

    auto call_local_by_value = [=](Int<8> x) -> Int<8> {
        return value_lambda(x, b) + Int<8>(2);
    };
    lambda_calls_lambda_value = call_local_by_value(a);

    auto call_local_by_ref = [&](Int<8> x) -> Int<8> {
        mutable_main = mutable_main ^ x;
        return value_lambda(mutable_main, b) ^ lambda_ref_mutation;
    };
    lambda_calls_lambda_ref = call_local_by_ref(chain);

    auto template_lambda = [&]<uint32_t IDX = 0>(Int<8> x) -> void {
        if constexpr (IDX == 0) {
            lambda_template_0 = x + b;
        } else if constexpr (IDX == 1) {
            lambda_template_1 = x ^ b;
        }
    };
    template_lambda.template operator()<0>(a);
    template_lambda.template operator()<1>(chain);

    helper_lambda = helper_with_local_lambda(a, alt);
    loop_inline = loop_calls_helper(b);

    Int<8> helper_side = helper_with_local_lambda(chain, alt);
    Int<8> loop_side = loop_calls_helper(a);
    return_mix = choose_path(sel, helper_side, loop_side);

    helper_global_read = read_global_input();
    helper_global_chain = transitive_global_input();
    write_global_output(helper_global_chain);
    helper_global_shadow = shadow_global_name(b);
    template_write_global<0>(add_bias(a) + Int<8>(1));
    template_write_global<0>(a);
    template_write_global<1>(b);
    helper_default_array = explicitly_initialized_array(a);
    std::array<Int<8>, 2> complex_values = {a, b};
    helper_complex_lambda_arg =
        value_lambda(complex_values[0] ^ complex_values[1], a);
}
