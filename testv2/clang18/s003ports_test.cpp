#include "s0clang18/s003ports.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

[[noreturn]] static void failCheck(const char* expr, const char* file, int line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expr << "\n";
    std::exit(1);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) failCheck(#condition, __FILE__, __LINE__); \
    } while (false)

static pred::s0clang18::Clang18Session buildSession(std::string source) {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s003_virtual_input.logic.cpp";
    options.source_text = std::move(source);
    options.top_function = "hls_main";
    options.clang_args = {"-I.", "-Ithird_party/vulsim/vullib"};

    auto result = pred::s0clang18::createClang18Session(options);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());
    return std::move(*result.value);
}

static bool hasError(const std::vector<pred::s0clang18::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == pred::s0clang18::Severity::Error) return true;
    }
    return false;
}

static void testCollectsValidPorts() {
    auto session = buildSession(R"cpp(
#include <array>
#include <cstdint>
#include <fixint.hpp>

#pragma input_port in_bit
bool in_bit;
#pragma input_port bus
Int<16> bus;
#pragma input_port lanes
std::array<uint8_t, 4> lanes;
#pragma output_port out_value
Int<17> out_value;

void hls_main() {}
)cpp");

    pred::s0clang18::SourceLocPolicy policy;
    policy.canonicalize_paths = false;
    auto result = pred::s0clang18::collectPragmaAndPortDecls(session, policy);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());
    const auto& table = *result.value;
    CHECK(table.pragmas.size() == 4);
    CHECK(table.ports.size() == 4);
    CHECK(table.port_by_name.at("in_bit") == 0);
    CHECK(table.port_by_name.at("bus") == 1);
    CHECK(table.port_by_name.at("lanes") == 2);
    CHECK(table.port_by_name.at("out_value") == 3);

    CHECK(table.ports[0].direction == pred::s0clang18::PortDirection::Input);
    CHECK(table.ports[0].type.name == "bool");
    CHECK(table.ports[1].type.hw_kind == "Int");
    CHECK(table.ports[1].type.width == 16);
    CHECK(table.ports[2].type.is_array);
    CHECK(table.ports[2].type.array_size == 4);
    CHECK(table.ports[2].type.array_dims.size() == 1);
    CHECK(table.ports[2].type.array_dims[0] == 4);
    CHECK(table.ports[2].type.hw_kind == "builtin");
    CHECK(table.ports[2].type.width == 8);
    CHECK(table.ports[3].direction == pred::s0clang18::PortDirection::Output);
    CHECK(pred::s0clang18::toV2Direction(table.ports[3].direction) ==
          pred::v2::ParamDirection::Output);
    CHECK(table.ports[3].decl_loc.valid());
    CHECK(table.ports[3].pragma_loc.valid());
}

static void testRejectsUnmarkedGlobal() {
    auto session = buildSession(R"cpp(
#include <fixint.hpp>
#pragma input_port marked
Int<8> marked;
Int<8> unmarked;
void hls_main() {}
)cpp");

    auto result = pred::s0clang18::collectPragmaAndPortDecls(session);
    CHECK(!result.ok());
    CHECK(!result.value.has_value());
    CHECK(hasError(result.diagnostics));
}

static void testRejectsInitializedPortAndMissingGlobal() {
    auto session = buildSession(R"cpp(
#include <fixint.hpp>
#pragma input_port initialized
Int<8> initialized = Int<8>(0);
#pragma output_port missing
void hls_main() {}
)cpp");

    auto result = pred::s0clang18::collectPragmaAndPortDecls(session);
    CHECK(!result.ok());
    CHECK(!result.value.has_value());
    CHECK(hasError(result.diagnostics));
}

int main() {
    testCollectsValidPorts();
    testRejectsUnmarkedGlobal();
    testRejectsInitializedPortAndMissingGlobal();
    std::cout << "s003ports_test passed\n";
    return 0;
}

