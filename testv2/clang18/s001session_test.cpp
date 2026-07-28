#include "s0clang18/s001session.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/Basic/LangOptions.h>

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

static bool hasError(const std::vector<pred::s0clang18::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == pred::s0clang18::Severity::Error) return true;
    }
    return false;
}

static pred::s0clang18::Clang18Options baseOptions(std::string source) {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s001_virtual_input.logic.cpp";
    options.source_text = std::move(source);
    options.top_function = "hls_main";
    options.clang_args = {"-I.", "-Ithird_party/vulsim/vullib"};
    return options;
}

static void testValidSourceBuildsSession() {
    auto options = baseOptions(R"cpp(
        constexpr int compute() {
            if constexpr (true) {
                return 42;
            } else {
                return 0;
            }
        }

        void hls_main() {
            constexpr int value = compute();
            (void)value;
        }
    )cpp");

    auto result = pred::s0clang18::createClang18Session(options);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());

    auto& session = *result.value;
    CHECK(session.ast_unit != nullptr);
    CHECK(session.ast_context != nullptr);
    CHECK(session.source_manager != nullptr);
    CHECK(session.translation_unit != nullptr);
    CHECK(session.main_file_id.isValid());
    CHECK(session.ast_context->getLangOpts().CPlusPlus20);
    CHECK(!hasError(session.diagnostics));
}

static void testExplicitStdArgIsAccepted() {
    auto options = baseOptions("void hls_main() {}\n");
    options.clang_args.push_back("-std=c++20");

    auto result = pred::s0clang18::createClang18Session(options);
    CHECK(result.ok());
    CHECK(result.value.has_value());
    CHECK(result.value->ast_context->getLangOpts().CPlusPlus20);
}

static void testInvalidSourceReportsError() {
    auto options = baseOptions("void hls_main( {\n");

    auto result = pred::s0clang18::createClang18Session(options);
    CHECK(!result.ok());
    CHECK(!result.value.has_value());
    CHECK(hasError(result.diagnostics));
}

int main() {
    testValidSourceBuildsSession();
    testExplicitStdArgIsAccepted();
    testInvalidSourceReportsError();
    std::cout << "s001session_test passed\n";
    return 0;
}

