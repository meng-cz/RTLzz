#include "s0clang18/s0clang18.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

[[noreturn]] static void failCheck(const char* expr, const char* file, int line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expr << "\n";
    std::exit(1);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) failCheck(#condition, __FILE__, __LINE__); \
    } while (false)

static pred::s0clang18::S0Clang18PipelineOptions makeOptions(
    const std::string& source_name,
    std::optional<std::string> source_text = std::nullopt) {
    pred::s0clang18::S0Clang18PipelineOptions options;
    options.clang.source_name = source_name;
    options.clang.source_text = std::move(source_text);
    options.clang.top_function = "hls_main";
    options.clang.clang_args = {"-I.", "-Ithird_party/vulsim/vullib"};
    options.loc_policy.canonicalize_paths = false;
    options.stop_after =
        pred::s0clang18::S0Clang18Step::GlobalPortLift;
    return options;
}

static void printDiagnostics(
    const std::vector<pred::s0clang18::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << "    " << pred::s0clang18::diagnosticLabel(diagnostic)
                  << "\n";
    }
}

static bool runOneFile(const std::string& path) {
    auto result = pred::s0clang18::runS0Clang18Pipeline(makeOptions(path));
    bool ok = result.ok();
    int completed_step = result.value ? result.value->completed_step : 0;
    std::cout << path << ": " << (ok ? "PASS" : "FAIL")
              << " completed_step=" << completed_step << "\n";
    if (!ok) {
        printDiagnostics(result.diagnostics);
        if (result.value) {
            std::cerr << result.value->intermediate_text << "\n";
        }
    }
    return ok;
}

static void runVirtualSmokeTest() {
    std::string source = R"cpp(
#include <fixint.hpp>

#pragma input_port in_value
Int<8> in_value;
#pragma output_port out_value
Int<8> out_value;

Int<8> helper() {
    return in_value;
}

void hls_main() {
    out_value = helper();
}
)cpp";

    auto result = pred::s0clang18::runS0Clang18Pipeline(
        makeOptions("testv2/clang18/s0clang18_api_virtual.logic.cpp", source));
    if (!result.ok()) {
        printDiagnostics(result.diagnostics);
        if (result.value) std::cerr << result.value->intermediate_text << "\n";
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());
    CHECK(result.value->completed_step == 16);
    CHECK(result.value->ports.has_value());
    CHECK(result.value->top.has_value());
    CHECK(result.value->semantic_index.has_value());
    CHECK(result.value->reachability.has_value());
    CHECK(result.value->template_specializations.has_value());
    CHECK(result.value->lambdas.has_value());
    CHECK(result.value->surface_function.has_value());
    CHECK(result.value->port_lift_plan.has_value());
    CHECK(!result.value->intermediate_text.empty());

    auto stop_after_ports = makeOptions(
        "testv2/clang18/s0clang18_api_virtual.logic.cpp", source);
    stop_after_ports.stop_after =
        pred::s0clang18::S0Clang18Step::PragmaAndPortDeclCollect;
    auto partial = pred::s0clang18::runS0Clang18Pipeline(stop_after_ports);
    CHECK(partial.ok());
    CHECK(partial.value.has_value());
    CHECK(partial.value->completed_step == 3);
    CHECK(partial.value->ports.has_value());
    CHECK(!partial.value->top.has_value());
}

int main(int argc, char** argv) {
    if (argc <= 1) {
        runVirtualSmokeTest();
        std::cout << "s0clang18_api_test passed\n";
        return 0;
    }

    bool all_ok = true;
    for (int i = 1; i < argc; ++i) {
        all_ok = runOneFile(argv[i]) && all_ok;
    }
    return all_ok ? 0 : 1;
}
