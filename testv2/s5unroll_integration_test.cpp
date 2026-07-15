#include "ast/ASTBuilder.h"
#include "s1apinorm/S1APINorm.h"
#include "s2validate/S2Validate.h"
#include "s3statementize/S3Statementize.h"
#include "s4cfg/S4CFG.h"
#include "s5unroll/S5Unroll.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace pred;

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
    auto build = buildASTFromSource(file, "hls_main", clang_args);
    if (!build.error.empty()) {
        std::cerr << "AST build failed for " << file << ":\n" << build.error << "\n";
    }
    CHECK(build.error.empty());
    CHECK(build.function.has_value());
    return std::move(build.function.value());
}

struct S5SourceRun {
    std::string debug;
    std::size_t summary_count = 0;
};

static S5SourceRun runS5FromSource(const std::string& file) {
    auto ast = parseFixture(file);
    auto s1 = pred::s1apinorm::normalizeAPIs(ast);
    if (!s1.ok()) std::cerr << s1.error->formatted << "\n";
    CHECK(s1.ok());
    CHECK(s1.function.has_value());

    auto validation = pred::s2validate::validateFunctionAST(s1.function.value());
    if (!validation.ok()) std::cerr << validation.error->formatted << "\n";
    CHECK(validation.ok());

    auto s3 = pred::s3statementize::statementizeFunctionAST(s1.function.value());
    if (!s3.ok()) std::cerr << s3.error->formatted << "\n";
    CHECK(s3.ok());
    CHECK(s3.program.has_value());

    auto s4 = pred::s4cfg::buildCFGProgram(s3.program.value());
    if (!s4.ok()) std::cerr << s4.error->formatted << "\n";
    CHECK(s4.ok());
    CHECK(s4.program.has_value());

    pred::s5unroll::UnrollOptions options;
    options.debug_print = true;
    auto s5 = pred::s5unroll::unrollCFGProgram(s4.program.value(), options);
    if (!s5.ok()) std::cerr << s5.error->formatted << "\n";
    CHECK(s5.ok());
    CHECK(s5.program.has_value());
    CHECK(!s5.debug_text.empty());
    CHECK(s5.program->top.loop_regions.empty());
    std::unordered_set<pred::s3statementize::SymbolId> symbol_ids;
    for (std::size_t i = 0; i < s5.program->top.symbols.size(); ++i) {
        CHECK(s5.program->top.symbols[i].id ==
              static_cast<pred::s3statementize::SymbolId>(i));
        CHECK(symbol_ids.insert(s5.program->top.symbols[i].id).second);
    }
    std::unordered_set<pred::s3statementize::SymbolId> declared_symbols;
    for (const auto& block : s5.program->top.blocks) {
        CHECK(block->loop_stack.empty());
        for (const auto& cfg_stmt : block->stmts) {
            if (!cfg_stmt.stmt ||
                cfg_stmt.stmt->kind != pred::s3statementize::S3StmtKind::Decl) {
                continue;
            }
            auto symbol = cfg_stmt.stmt->decl_symbol;
            CHECK(symbol >= 0);
            CHECK(symbol <
                  static_cast<pred::s3statementize::SymbolId>(
                      s5.program->top.symbols.size()));
            CHECK(cfg_stmt.stmt->decl_name ==
                  s5.program->top.symbols[static_cast<std::size_t>(symbol)].name);
            CHECK(declared_symbols.insert(symbol).second);
        }
    }
    return S5SourceRun{s5.debug_text, s5.summaries.size()};
}

static void expectContains(const std::string& text, const std::string& needle) {
    if (text.find(needle) == std::string::npos) {
        std::cerr << "Expected debug text to contain:\n" << needle
                  << "\nActual debug text:\n" << text << "\n";
    }
    CHECK(text.find(needle) != std::string::npos);
}

static void expectNotContains(const std::string& text, const std::string& needle) {
    if (text.find(needle) != std::string::npos) {
        std::cerr << "Expected debug text not to contain:\n" << needle
                  << "\nActual debug text:\n" << text << "\n";
    }
    CHECK(text.find(needle) == std::string::npos);
}

static std::size_t countSubstring(const std::string& text, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

static void sourceLoopsUnrollThroughS5() {
    auto run = runS5FromSource(
        "testv2/fixtures/s5unroll/source_control.logic.cpp");
    const auto& debug = run.debug;

    CHECK(run.summary_count == 6);
    CHECK(countSubstring(debug, "unrolled hls_main loop=") == 6);
    expectContains(debug, "iterations=2");
    expectContains(debug, "iterations=3");
    expectContains(debug, "iterations=4");
    expectContains(debug, "iterations=5");
    expectContains(debug, "Eq(1,");
    expectContains(debug, "Eq(2,");
    expectContains(debug, "Eq(3,");
    expectContains(debug, "decl __s5_loop_enable_");
    expectContains(debug, "assign __s5_loop_enable_");
    expectContains(debug, " = false");
    expectContains(debug, "term branch __s5_loop_enable_");
    expectContains(debug, "term branch stop ?");
    expectNotContains(debug, " loop 0 pre_test ");
    expectNotContains(debug, "backedge");
    expectNotContains(debug, "continue:");
}

static void sourceNestedDynamicControlUnrollsThroughS5() {
    auto run = runS5FromSource(
        "testv2/fixtures/s5unroll/source_nested_dynamic_control.logic.cpp");
    const auto& debug = run.debug;

    CHECK(run.summary_count == 2);
    CHECK(countSubstring(debug, "unrolled hls_main loop=") == 2);
    expectContains(debug, "iterations=2");
    expectContains(debug, "iterations=3");
    CHECK(countSubstring(debug, "decl __s5_loop_enable_") >= 2);
    CHECK(countSubstring(debug, " = false") >= 2);
    expectContains(debug, "term branch skip_outer ?");
    expectContains(debug, "term branch stop_outer ?");
    expectContains(debug, "term branch skip_inner ?");
    expectContains(debug, "term branch stop_inner ?");
    expectNotContains(debug, "backedge");
    expectNotContains(debug, "continue:");
}

int main() {
    sourceLoopsUnrollThroughS5();
    sourceNestedDynamicControlUnrollsThroughS5();
    return 0;
}
