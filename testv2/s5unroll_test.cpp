#include "v2/V2AST.h"
#include "s1apinorm/S1APINorm.h"
#include "s3statementize/S3Statementize.h"
#include "s4cfg/S4CFG.h"
#include "s5unroll/S5Unroll.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_set>
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

static TypeInfo uint32Type() {
    TypeInfo type;
    type.name = "uint32_t";
    type.width = 32;
    type.is_signed = false;
    type.hw_kind = "builtin";
    return type;
}

static pred::s1apinorm::S1FunctionAST normalizeS1OrFail(const FunctionAST& fn) {
    auto result = pred::s1apinorm::normalizeAPIs(fn);
    if (!result.ok()) std::cerr << result.error->formatted << "\n";
    CHECK(result.ok());
    CHECK(result.function.has_value());
    return std::move(result.function.value());
}

static TypeInfo boolType() {
    return make_bool_type();
}

static TypeInfo voidType() {
    TypeInfo type;
    type.name = "void";
    return type;
}

static ParamDecl outputParam(const std::string& name, TypeInfo type) {
    ParamDecl out;
    out.name = name;
    out.type = std::move(type);
    out.type.is_reference = true;
    out.passing = ParamPassingKind::MutableRef;
    out.direction = ParamDirection::Output;
    out.is_reference = true;
    out.is_output = true;
    return out;
}

static ParamDecl inputParam(const std::string& name, TypeInfo type) {
    ParamDecl out;
    out.name = name;
    out.type = std::move(type);
    out.passing = ParamPassingKind::Value;
    out.direction = ParamDirection::Input;
    return out;
}

static StmtPtr assign(ExprPtr target, ExprPtr value) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Assign;
    stmt->assign_target = std::move(target);
    stmt->assign_value = std::move(value);
    return stmt;
}

static StmtPtr decl(const std::string& name, TypeInfo type, ExprPtr init = nullptr) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Decl;
    stmt->decl_name = name;
    stmt->decl_type = std::move(type);
    if (init) stmt->decl_init = std::move(init);
    return stmt;
}

static StmtPtr ifStmt(ExprPtr cond, std::vector<StmtPtr> then_body) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::If;
    stmt->if_cond = std::move(cond);
    stmt->if_then = std::move(then_body);
    return stmt;
}

static StmtPtr forStmt(const std::string& var,
                       int begin,
                       int end,
                       std::vector<StmtPtr> body) {
    auto type = uint32Type();
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::For;
    stmt->for_init = decl(var, type, make_literal(std::to_string(begin), type));
    stmt->for_cond = make_binary("<",
                                 make_var(var, type),
                                 make_literal(std::to_string(end), type),
                                 boolType());
    stmt->for_step = make_unary("++", make_var(var, type), type);
    stmt->for_body = std::move(body);
    return stmt;
}

static StmtPtr breakStmt() {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Break;
    return stmt;
}

static StmtPtr continueStmt() {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Continue;
    return stmt;
}

static ExprPtr eqVarLiteral(const std::string& var, int value) {
    auto type = uint32Type();
    return make_binary("==",
                       make_var(var, type),
                       make_literal(std::to_string(value), type),
                       boolType());
}

static FunctionAST baseTop() {
    FunctionAST top;
    top.name = "hls_main";
    top.return_type = voidType();
    top.params.push_back(outputParam("out", uint32Type()));
    return top;
}

struct S5Run {
    pred::s5unroll::UnrollResult result;
    std::string debug;
};

static S5Run runS5(const FunctionAST& fn) {
    auto s1 = normalizeS1OrFail(fn);
    auto s3 = pred::s3statementize::statementizeFunctionASTOrThrow(s1);
    auto s4 = pred::s4cfg::buildCFGProgram(s3);
    CHECK(s4.ok());
    CHECK(s4.program.has_value());

    pred::s5unroll::UnrollOptions options;
    options.debug_print = true;
    auto s5 = pred::s5unroll::unrollCFGProgram(s4.program.value(), options);
    if (!s5.ok()) std::cerr << s5.error->formatted << "\n";
    CHECK(s5.ok());
    CHECK(s5.program.has_value());
    CHECK(!s5.debug_text.empty());
    std::string debug = s5.debug_text;
    return S5Run{std::move(s5), std::move(debug)};
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

static void expectBreakEnableFlag(const std::string& debug) {
    expectContains(debug, "decl __s5_loop_enable_");
    expectContains(debug, "assign __s5_loop_enable_");
    expectContains(debug, " = false");
    expectContains(debug, "term branch __s5_loop_enable_");
}

static void verifyNoLoopMetadata(const pred::s4cfg::CFGProgram& program) {
    auto verify_fn = [](const pred::s4cfg::FunctionCFG& fn) {
        CHECK(fn.loop_regions.empty());
        std::unordered_set<pred::s3statementize::SymbolId> symbol_ids;
        for (std::size_t i = 0; i < fn.symbols.size(); ++i) {
            CHECK(fn.symbols[i].id == static_cast<pred::s3statementize::SymbolId>(i));
            CHECK(symbol_ids.insert(fn.symbols[i].id).second);
        }
        std::unordered_set<pred::s3statementize::SymbolId> declared_symbols;
        for (const auto& block : fn.blocks) {
            CHECK(block->loop_stack.empty());
            for (const auto& cfg_stmt : block->stmts) {
                if (!cfg_stmt.stmt ||
                    cfg_stmt.stmt->kind != pred::s3statementize::S3StmtKind::Decl) {
                    continue;
                }
                auto symbol = cfg_stmt.stmt->decl_symbol;
                CHECK(symbol >= 0);
                CHECK(symbol < static_cast<pred::s3statementize::SymbolId>(fn.symbols.size()));
                CHECK(cfg_stmt.stmt->decl_name == fn.symbols[static_cast<std::size_t>(symbol)].name);
                CHECK(declared_symbols.insert(symbol).second);
            }
            for (const auto& edge : block->successors) {
                CHECK(edge.label != "backedge");
                CHECK(edge.kind != pred::s4cfg::EdgeKind::Continue);
            }
        }
    };
    verify_fn(program.top);
    for (const auto& helper : program.helpers) verify_fn(helper);
    for (const auto& [_, lambda] : program.lambdas) verify_fn(lambda);
}

static void simpleLoopUnrollsToLiteralIterations() {
    auto top = baseTop();
    top.body.push_back(forStmt("i", 0, 3, {
        assign(make_var("out", uint32Type()), make_var("i", uint32Type())),
    }));

    auto run = runS5(top);
    CHECK(run.result.summaries.size() == 1);
    CHECK(run.result.summaries[0].iterations == 3);
    verifyNoLoopMetadata(run.result.program.value());
    expectContains(run.debug, "unrolled hls_main loop=0 kind=pre_test iterations=3");
    expectContains(run.debug, "assign out = 0");
    expectContains(run.debug, "assign out = 1");
    expectContains(run.debug, "assign out = 2");
    expectNotContains(run.debug, "backedge");
}

static void nestedLoopsAreUnrolledInsideOut() {
    auto top = baseTop();
    top.body.push_back(forStmt("i", 0, 2, {
        forStmt("j", 0, 3, {
            assign(make_var("out", uint32Type()),
                   make_binary("+",
                               make_var("i", uint32Type()),
                               make_var("j", uint32Type()),
                               uint32Type())),
        }),
    }));

    auto run = runS5(top);
    CHECK(run.result.summaries.size() == 2);
    CHECK(run.result.summaries[0].iterations == 3);
    CHECK(run.result.summaries[1].iterations == 2);
    verifyNoLoopMetadata(run.result.program.value());
    expectContains(run.debug, "Add(0, 0)");
    expectContains(run.debug, "Add(0, 2)");
    expectContains(run.debug, "Add(1, 0)");
    expectContains(run.debug, "Add(1, 2)");
}

static void loopDerivedVariableChainPropagatesAcrossBlocks() {
    auto top = baseTop();
    top.params.insert(top.params.begin(), inputParam("take", boolType()));
    top.body.push_back(forStmt("i", 0, 3, {
        decl("idx", uint32Type(), make_var("i", uint32Type())),
        ifStmt(make_var("take", boolType()), {
            assign(make_var("out", uint32Type()),
                   make_var("idx", uint32Type())),
        }),
    }));

    auto run = runS5(top);
    CHECK(run.result.summaries.size() == 1);
    CHECK(run.result.summaries[0].iterations == 3);
    verifyNoLoopMetadata(run.result.program.value());
    expectContains(run.debug, "assign out = 0");
    expectContains(run.debug, "assign out = 1");
    expectContains(run.debug, "assign out = 2");
}

static void nestedDynamicContinueAndBreakAreMasked() {
    auto top = baseTop();
    top.params.insert(top.params.begin(), inputParam("stop_inner", boolType()));
    top.params.insert(top.params.begin(), inputParam("skip_inner", boolType()));
    top.params.insert(top.params.begin(), inputParam("stop_outer", boolType()));
    top.params.insert(top.params.begin(), inputParam("skip_outer", boolType()));
    top.body.push_back(forStmt("i", 0, 2, {
        ifStmt(make_var("skip_outer", boolType()), {continueStmt()}),
        ifStmt(make_var("stop_outer", boolType()), {breakStmt()}),
        forStmt("j", 0, 3, {
            ifStmt(make_var("skip_inner", boolType()), {continueStmt()}),
            ifStmt(make_var("stop_inner", boolType()), {breakStmt()}),
            assign(make_var("out", uint32Type()),
                   make_binary("+",
                               make_var("i", uint32Type()),
                               make_var("j", uint32Type()),
                               uint32Type())),
        }),
    }));

    auto run = runS5(top);
    CHECK(run.result.summaries.size() == 2);
    CHECK(run.result.summaries[0].iterations == 3);
    CHECK(run.result.summaries[1].iterations == 2);
    verifyNoLoopMetadata(run.result.program.value());
    CHECK(countSubstring(run.debug, "decl __s5_loop_enable_") >= 2);
    CHECK(countSubstring(run.debug, " = false") >= 2);
    expectContains(run.debug, "term branch skip_outer ?");
    expectContains(run.debug, "term branch stop_outer ?");
    expectContains(run.debug, "term branch skip_inner ?");
    expectContains(run.debug, "term branch stop_inner ?");
    expectContains(run.debug, "Add(0, 0)");
    expectContains(run.debug, "Add(0, 2)");
    expectContains(run.debug, "Add(1, 0)");
    expectContains(run.debug, "Add(1, 2)");
    expectNotContains(run.debug, "continue:");
    expectNotContains(run.debug, "backedge");
}

static void breakInLoopBodyKeepsExitPathAndRemovesBackedge() {
    auto top = baseTop();
    top.body.push_back(forStmt("i", 0, 5, {
        ifStmt(eqVarLiteral("i", 2), {breakStmt()}),
        assign(make_var("out", uint32Type()), make_var("i", uint32Type())),
    }));

    auto run = runS5(top);
    CHECK(run.result.summaries.size() == 1);
    CHECK(run.result.summaries[0].iterations == 5);
    verifyNoLoopMetadata(run.result.program.value());
    expectBreakEnableFlag(run.debug);
    expectContains(run.debug, "Eq(2,");
    expectContains(run.debug, "assign out = 0");
    expectContains(run.debug, "assign out = 4");
    expectNotContains(run.debug, "backedge");
}

static void dynamicBreakInBranchMasksLaterIterations() {
    auto top = baseTop();
    top.params.insert(top.params.begin(), inputParam("stop", boolType()));
    top.body.push_back(forStmt("i", 0, 4, {
        ifStmt(make_var("stop", boolType()), {breakStmt()}),
        assign(make_var("out", uint32Type()), make_var("i", uint32Type())),
    }));

    auto run = runS5(top);
    CHECK(run.result.summaries.size() == 1);
    CHECK(run.result.summaries[0].iterations == 4);
    verifyNoLoopMetadata(run.result.program.value());
    expectBreakEnableFlag(run.debug);
    expectContains(run.debug, "term branch stop ?");
    expectContains(run.debug, "assign out = 0");
    expectContains(run.debug, "assign out = 3");
    expectNotContains(run.debug, "backedge");
}

static void continueInLoopBodyTargetsNextUnrolledIteration() {
    auto top = baseTop();
    top.body.push_back(forStmt("i", 0, 4, {
        ifStmt(eqVarLiteral("i", 1), {continueStmt()}),
        assign(make_var("out", uint32Type()), make_var("i", uint32Type())),
    }));

    auto run = runS5(top);
    CHECK(run.result.summaries.size() == 1);
    CHECK(run.result.summaries[0].iterations == 4);
    verifyNoLoopMetadata(run.result.program.value());
    expectContains(run.debug, "Eq(1,");
    expectContains(run.debug, "assign out = 0");
    expectContains(run.debug, "assign out = 3");
    expectNotContains(run.debug, "continue:");
    expectNotContains(run.debug, "backedge");
}

static void breakAndContinueInBranchesAreBothHandled() {
    auto top = baseTop();
    top.body.push_back(forStmt("i", 0, 5, {
        ifStmt(eqVarLiteral("i", 1), {continueStmt()}),
        ifStmt(eqVarLiteral("i", 3), {breakStmt()}),
        assign(make_var("out", uint32Type()), make_var("i", uint32Type())),
    }));

    auto run = runS5(top);
    CHECK(run.result.summaries.size() == 1);
    CHECK(run.result.summaries[0].iterations == 5);
    verifyNoLoopMetadata(run.result.program.value());
    expectBreakEnableFlag(run.debug);
    expectContains(run.debug, "Eq(1,");
    expectContains(run.debug, "Eq(3,");
    expectContains(run.debug, "assign out = 0");
    expectContains(run.debug, "assign out = 4");
    expectNotContains(run.debug, "continue:");
    expectNotContains(run.debug, "backedge");
}

int main() {
    simpleLoopUnrollsToLiteralIterations();
    nestedLoopsAreUnrolledInsideOut();
    loopDerivedVariableChainPropagatesAcrossBlocks();
    nestedDynamicContinueAndBreakAreMasked();
    breakInLoopBodyKeepsExitPathAndRemovesBackedge();
    dynamicBreakInBranchMasksLaterIterations();
    continueInLoopBodyTargetsNextUnrolledIteration();
    breakAndContinueInBranchesAreBothHandled();
    return 0;
}
