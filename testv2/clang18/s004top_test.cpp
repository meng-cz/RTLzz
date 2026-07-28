#include "s0clang18/s004top.hpp"

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
    options.source_name = "testv2/clang18/s004_virtual_input.logic.cpp";
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

static void testExactTopSelection() {
    auto session = buildSession(R"cpp(
void helper() {}
void hls_main() {}
)cpp");

    auto result = pred::s0clang18::selectTopFunction(session, "hls_main");
    CHECK(result.ok());
    CHECK(result.value.has_value());
    CHECK(result.value->requested_name == "hls_main");
    CHECK(result.value->resolved_name == "hls_main");
    CHECK(result.value->function_decl != nullptr);
    CHECK(result.value->loc.valid());

    auto candidates =
        pred::s0clang18::collectTopFunctionCandidates(session, "hls_*");
    CHECK(candidates.size() == 1);
    CHECK(candidates[0].name == "hls_main");
}

static void testWildcardTopSelection() {
    auto session = buildSession(R"cpp(
void helper() {}
void LogicSubModule_ALU() {}
)cpp");

    auto result =
        pred::s0clang18::selectTopFunction(session, "LogicSubModule_*");
    CHECK(result.ok());
    CHECK(result.value.has_value());
    CHECK(result.value->resolved_name == "LogicSubModule_ALU");
}

static void testRejectsMultipleWildcardMatches() {
    auto session = buildSession(R"cpp(
void LogicSubModule_A() {}
void LogicSubModule_B() {}
)cpp");

    auto result =
        pred::s0clang18::selectTopFunction(session, "LogicSubModule_*");
    CHECK(!result.ok());
    CHECK(!result.value.has_value());
    CHECK(hasError(result.diagnostics));
}

static void testRejectsBadSignaturesAndMissingTop() {
    {
        auto session = buildSession("int hls_main() { return 0; }\n");
        auto result = pred::s0clang18::selectTopFunction(session, "hls_main");
        CHECK(!result.ok());
        CHECK(hasError(result.diagnostics));
    }
    {
        auto session = buildSession("void hls_main(int x) { (void)x; }\n");
        auto result = pred::s0clang18::selectTopFunction(session, "hls_main");
        CHECK(!result.ok());
        CHECK(hasError(result.diagnostics));
    }
    {
        auto session = buildSession("void other() {}\n");
        auto result = pred::s0clang18::selectTopFunction(session, "hls_main");
        CHECK(!result.ok());
        CHECK(hasError(result.diagnostics));
    }
}

int main() {
    testExactTopSelection();
    testWildcardTopSelection();
    testRejectsMultipleWildcardMatches();
    testRejectsBadSignaturesAndMissingTop();
    std::cout << "s004top_test passed\n";
    return 0;
}

