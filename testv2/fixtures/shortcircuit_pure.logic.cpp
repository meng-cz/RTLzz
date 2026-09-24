#include <cstdint>
#include <fixint.hpp>

#pragma input_port gate
bool gate;
#pragma input_port a
bool a;
#pragma input_port b
bool b;
#pragma input_port lookup_slot
uint8_t lookup_slot;
#pragma output_port selected
bool selected;
#pragma output_port or_result
bool or_result;
#pragma output_port and_result
bool and_result;
#pragma output_port or_calls
bool or_calls;
#pragma output_port and_calls
bool and_calls;
#pragma output_port protected_lookup
bool protected_lookup;

bool effect_or() { or_calls = true; return b; }
bool effect_and() { and_calls = true; return b; }

void hls_main() {
    selected = 0;
    or_calls = 0;
    and_calls = 0;
    if (gate) {
        bool local = a || (b && !a);
        selected = local;
    }
    or_result = a || effect_or();
    and_result = a && effect_and();
    const uint8_t table[4] = {1, 2, 0, 0};
    protected_lookup = lookup_slot >= 4U || table[lookup_slot] != 0U;
}
