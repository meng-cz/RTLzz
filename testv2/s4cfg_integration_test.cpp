#include "s0ast/S0AST.h"
#include "s1apinorm/S1APINorm.h"
#include "s2validate/S2Validate.h"
#include "s3statementize/S3Statementize.h"
#include "s4cfg/S4CFG.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace pred;
using namespace pred::v2;

[[noreturn]] static void failCheck(const char* expr, const char* file, int line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expr << "\n";
    std::exit(1);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) failCheck(#condition, __FILE__, __LINE__); \
    } while (false)

static FunctionAST parseFixture(const std::string& file) {
    std::vector<std::string> clang_args = {
        "-I.",
        "-Ithird_party/vulsim/vullib",
        "-std=c++20",
    };
    auto parsed = pred::s0ast::parseProgram(file, std::nullopt, "hls_main", clang_args);
    if (!parsed.ok()) {
        std::cerr << "S0 parse failed for " << file << ":\n"
                  << (parsed.error ? parsed.error->message : "unknown error") << "\n";
    }
    CHECK(parsed.ok());
    CHECK(parsed.program.has_value());
    return pred::s0ast::surfaceAST(*parsed.program);
}

static std::string buildS4DebugFromSource(const std::string& file) {
    auto ast = parseFixture(file);
    auto s1 = pred::s1apinorm::normalizeAPIs(ast);
    if (!s1.ok()) {
        std::cerr << s1.error->formatted << "\n";
    }
    CHECK(s1.ok());
    CHECK(s1.function.has_value());

    pred::s2validate::ValidateOptions validate_options;
    validate_options.debug_print = true;
    auto validation = pred::s2validate::validateFunctionAST(s1.function.value(), validate_options);
    if (!validation.ok()) {
        std::cerr << validation.error->formatted << "\n";
    }
    CHECK(validation.ok());

    pred::s3statementize::StatementizeOptions s3_options;
    s3_options.debug_print = true;
    auto s3 = pred::s3statementize::statementizeFunctionAST(s1.function.value(), s3_options);
    if (!s3.ok()) {
        std::cerr << s3.error->formatted << "\n";
    }
    CHECK(s3.ok());
    CHECK(s3.program.has_value());

    pred::s4cfg::CFGOptions s4_options;
    s4_options.debug_print = true;
    auto s4 = pred::s4cfg::buildCFGProgram(s3.program.value(), s4_options);
    if (!s4.ok()) {
        std::cerr << s4.error->formatted << "\n";
    }
    CHECK(s4.ok());
    CHECK(s4.program.has_value());
    CHECK(!s4.debug_text.empty());
    return s4.debug_text;
}

static void expectContains(const std::string& text, const std::string& needle) {
    if (text.find(needle) == std::string::npos) {
        std::cerr << "Expected debug text to contain:\n" << needle
                  << "\nActual debug text:\n" << text << "\n";
    }
    CHECK(text.find(needle) != std::string::npos);
}

static void expectInOrder(const std::string& text, const std::vector<std::string>& needles) {
    std::size_t pos = 0;
    for (const auto& needle : needles) {
        auto found = text.find(needle, pos);
        if (found == std::string::npos) {
            std::cerr << "Expected in-order item:\n" << needle
                      << "\nActual debug text:\n" << text << "\n";
        }
        CHECK(found != std::string::npos);
        pos = found + needle.size();
    }
}

static void sourceHelpersBuildFunctionCFGs() {
    auto debug = buildS4DebugFromSource(
        "testv2/fixtures/s4cfg/source_helpers.logic.cpp");

    expectContains(debug, "s4cfg\n");
    expectContains(debug, "top hls_main entry=bb0 exit=bb2");
    expectContains(debug, "helper choose");
    expectContains(debug, "helper adjust");
    expectContains(debug, "helper touch");
    expectContains(debug, "return_slot=__ret_choose_0");
    expectContains(debug, "return_slot=__ret_adjust_0");
    expectContains(debug, "call t = choose(sel, a, b)");
    expectContains(debug, "call touch(t)");
    expectContains(debug, "call out = adjust(sel, t)");
    expectContains(debug, "term branch sel ?");
    expectContains(debug, "assign __ret_choose_0");
    expectContains(debug, "assign __ret_adjust_0");
    expectContains(debug, "term exit");
}

static void sourceLoopsSwitchBuildsEdgesAndFallthrough() {
    auto debug = buildS4DebugFromSource(
        "testv2/fixtures/s4cfg/source_loops_switch.logic.cpp");

    expectContains(debug, "loop 0 pre_test");
    expectContains(debug, "condition_prelude=bb");
    expectContains(debug, "loops=[0]");
    expectContains(debug, "continue");
    expectContains(debug, "break");
    expectContains(debug, "backedge");
    expectContains(debug, "term switch");
    expectContains(debug, "case 0");
    expectContains(debug, "case 1");
    expectContains(debug, "default ->");
}

static void sourceWhileDoWhileReevaluatesConditionPreludes() {
    auto debug = buildS4DebugFromSource(
        "testv2/fixtures/s4cfg/source_while_dowhile.logic.cpp");

    expectContains(debug, "helper keep");
    expectContains(debug, "helper again");
    expectContains(debug, "loop 0 pre_test");
    expectContains(debug, "loop 1 post_test");
    expectContains(debug, "condition_prelude=bb");
    expectContains(debug, "call __tmp_hls_main_keep_");
    expectContains(debug, "call __tmp_hls_main_again_");
    expectContains(debug, "continue");
    expectContains(debug, "break");
    expectInOrder(debug, {
        "loop 0 pre_test",
        "call __tmp_hls_main_keep_",
        "term branch __tmp_hls_main_keep_",
    });
    expectInOrder(debug, {
        "loop 1 post_test",
        "call __tmp_hls_main_again_",
        "term branch __tmp_hls_main_again_",
    });
}

int main() {
    sourceHelpersBuildFunctionCFGs();
    sourceLoopsSwitchBuildsEdgesAndFallthrough();
    sourceWhileDoWhileReevaluatesConditionPreludes();
    return 0;
}
