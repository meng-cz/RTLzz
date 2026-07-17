#include "s0ast/S0AST.h"
#include "s1apinorm/S1APINorm.h"
#include "s2validate/S2Validate.h"
#include "s3statementize/S3Statementize.h"
#include "s4cfg/S4CFG.h"
#include "s5unroll/S5Unroll.h"
#include "s6inline/S6Inline.h"
#include "s7flatten/S7Flatten.h"
#include "s8opnorm/S8Norm.h"
#include "s9ssa/S9SSA.h"
#include "s10predicate/S10Predicate.h"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
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
        std::cerr << (parsed.error ? parsed.error->message : "unknown error") << "\n";
    }
    CHECK(parsed.ok());
    CHECK(parsed.program.has_value());
    return pred::s0ast::surfaceAST(*parsed.program);
}

static s8opnorm::S8Type intType(int width) {
    return s8opnorm::S8Type{s8opnorm::S8TypeKind::Int, width};
}

static s8opnorm::S8Type boolType() {
    return s8opnorm::S8Type{s8opnorm::S8TypeKind::Bool, 1};
}

static s8opnorm::S8Literal literalBits(std::uint64_t value,
                                       s8opnorm::S8Type type,
                                       bool is_signed = false) {
    s8opnorm::S8Literal literal;
    literal.valid_width = type.width;
    literal.is_signed = is_signed;
    literal.source_text = std::to_string(value);
    literal.words.push_back(type.width >= 64 ? value : (value & ((std::uint64_t{1} << type.width) - 1)));
    return literal;
}

static s8opnorm::S8Operand var(s8opnorm::SymbolId symbol,
                               s8opnorm::S8Type type,
                               bool signed_view = false) {
    s8opnorm::S8Operand operand;
    operand.kind = s8opnorm::S8OperandKind::Var;
    operand.symbol = symbol;
    operand.type = type;
    operand.signed_view = signed_view;
    return operand;
}

static s8opnorm::S8Operand lit(std::uint64_t value,
                               s8opnorm::S8Type type,
                               bool signed_view = false) {
    s8opnorm::S8Operand operand;
    operand.kind = s8opnorm::S8OperandKind::Literal;
    operand.type = type;
    operand.signed_view = signed_view;
    operand.literal = literalBits(value, type, signed_view);
    return operand;
}

static s8opnorm::S8Symbol symbol(s8opnorm::SymbolId id,
                                 std::string name,
                                 s8opnorm::S8Type type,
                                 s8opnorm::S8SymbolRole role) {
    s8opnorm::S8Symbol out;
    out.id = id;
    out.debug_name = std::move(name);
    out.type = type;
    out.role = role;
    return out;
}

static s8opnorm::S8Stmt assign(s8opnorm::SymbolId target,
                               s8opnorm::S8Operand value) {
    s8opnorm::S8Stmt stmt;
    stmt.kind = s8opnorm::S8StmtKind::Assign;
    stmt.target = target;
    stmt.value = std::move(value);
    return stmt;
}

static s8opnorm::S8Stmt addOp(s8opnorm::SymbolId target,
                              s8opnorm::S8Operand lhs,
                              s8opnorm::S8Operand rhs,
                              int width) {
    s8opnorm::S8Stmt stmt;
    stmt.kind = s8opnorm::S8StmtKind::Op;
    stmt.target = target;
    stmt.op.kind = s8opnorm::S8OpKind::Add;
    stmt.op.result_width = width;
    stmt.op.operands = {std::move(lhs), std::move(rhs)};
    return stmt;
}

static s8opnorm::S8Stmt lookupStmt(s8opnorm::SymbolId target,
                                   s8opnorm::S8Operand index,
                                   std::vector<s8opnorm::S8Operand> elements) {
    s8opnorm::S8Stmt stmt;
    stmt.kind = s8opnorm::S8StmtKind::Lookup;
    stmt.target = target;
    stmt.lookup_index = std::move(index);
    stmt.lookup_elements = std::move(elements);
    return stmt;
}

static s8opnorm::S8Terminator jump(int target) {
    s8opnorm::S8Terminator term;
    term.kind = s8opnorm::S8TermKind::Jump;
    term.jump_target = target;
    return term;
}

static s8opnorm::S8Terminator exitTerm() {
    s8opnorm::S8Terminator term;
    term.kind = s8opnorm::S8TermKind::Exit;
    return term;
}

static s8opnorm::S8Terminator branch(s8opnorm::S8Operand cond,
                                     int true_target,
                                     int false_target) {
    s8opnorm::S8Terminator term;
    term.kind = s8opnorm::S8TermKind::Branch;
    term.condition = std::move(cond);
    term.true_target = true_target;
    term.false_target = false_target;
    return term;
}

static s8opnorm::S8NormProgram baseProgram() {
    s8opnorm::S8NormProgram program;
    auto& fn = program.top;
    fn.name = "hls_main";
    fn.entry = 0;
    fn.exit = 0;
    fn.symbols.push_back(symbol(0, "a", intType(8), s8opnorm::S8SymbolRole::Port));
    fn.symbols.push_back(symbol(1, "cond", boolType(), s8opnorm::S8SymbolRole::Port));
    fn.symbols.push_back(symbol(2, "out", intType(8), s8opnorm::S8SymbolRole::Port));
    fn.symbols.push_back(symbol(3, "idx", intType(2), s8opnorm::S8SymbolRole::Port));
    fn.symbols.push_back(symbol(4, "tmp", intType(8), s8opnorm::S8SymbolRole::Local));
    fn.ports.push_back(s8opnorm::S8Port{0, ParamDirection::Input, ParamPassingKind::Value});
    fn.ports.push_back(s8opnorm::S8Port{1, ParamDirection::Input, ParamPassingKind::Value});
    fn.ports.push_back(s8opnorm::S8Port{2, ParamDirection::Output, ParamPassingKind::MutableRef});
    fn.ports.push_back(s8opnorm::S8Port{3, ParamDirection::Input, ParamPassingKind::Value});
    s8opnorm::S8BasicBlock block;
    block.id = 0;
    block.terminator = exitTerm();
    fn.blocks.push_back(std::move(block));
    return program;
}

static std::string lowerDebug(const s8opnorm::S8NormProgram& program) {
    auto s9 = s9ssa::buildSSA(program);
    if (!s9.ok()) std::cerr << s9.error->formatted << "\n";
    CHECK(s9.ok());
    CHECK(s9.program.has_value());
    s10predicate::PredicateOptions options;
    options.debug_print = true;
    auto s10 = s10predicate::lowerPredicates(s9.program.value(), options);
    if (!s10.ok()) std::cerr << s10.error->formatted << "\n";
    CHECK(s10.ok());
    CHECK(s10.program.has_value());
    s10predicate::verifyPredicateProgram(s10.program.value());
    CHECK(!s10.debug_text.empty());
    return s10.debug_text;
}

static std::string runSourceToS10(const std::string& file) {
    auto ast = parseFixture(file);
    auto s1 = s1apinorm::normalizeAPIs(ast);
    if (!s1.ok()) std::cerr << s1.error->formatted << "\n";
    CHECK(s1.ok());
    CHECK(s1.function.has_value());
    auto s2 = s2validate::validateFunctionAST(s1.function.value());
    if (!s2.ok()) std::cerr << s2.error->formatted << "\n";
    CHECK(s2.ok());
    auto s3 = s3statementize::statementizeFunctionAST(s1.function.value());
    if (!s3.ok()) std::cerr << s3.error->formatted << "\n";
    CHECK(s3.ok());
    CHECK(s3.program.has_value());
    auto s4 = s4cfg::buildCFGProgram(s3.program.value());
    if (!s4.ok()) std::cerr << s4.error->formatted << "\n";
    CHECK(s4.ok());
    CHECK(s4.program.has_value());
    auto s5 = s5unroll::unrollCFGProgram(s4.program.value());
    if (!s5.ok()) std::cerr << s5.error->formatted << "\n";
    CHECK(s5.ok());
    CHECK(s5.program.has_value());
    auto s6 = s6inline::inlineCFGProgram(s5.program.value());
    if (!s6.ok()) std::cerr << s6.error->formatted << "\n";
    CHECK(s6.ok());
    CHECK(s6.program.has_value());
    auto s7 = s7flatten::flattenProgram(s6.program.value());
    if (!s7.ok()) std::cerr << s7.error->formatted << "\n";
    CHECK(s7.ok());
    CHECK(s7.program.has_value());
    auto s8 = s8opnorm::normalizeOperations(s7.program.value());
    if (!s8.ok()) std::cerr << s8.error->formatted << "\n";
    CHECK(s8.ok());
    CHECK(s8.program.has_value());
    return lowerDebug(s8.program.value());
}

static s8opnorm::S8NormProgram branchProgram() {
    auto program = baseProgram();
    auto& fn = program.top;
    fn.exit = 3;
    fn.blocks.clear();
    fn.blocks.resize(4);
    for (int i = 0; i < 4; ++i) fn.blocks[static_cast<std::size_t>(i)].id = i;
    fn.blocks[0].terminator = branch(var(1, boolType()), 1, 2);
    fn.blocks[1].stmts.push_back(assign(2, lit(1, intType(8))));
    fn.blocks[1].terminator = jump(3);
    fn.blocks[2].stmts.push_back(assign(2, lit(2, intType(8))));
    fn.blocks[2].terminator = jump(3);
    fn.blocks[3].terminator = exitTerm();
    return program;
}

static void straightLineUsesTrueGuard() {
    auto program = baseProgram();
    auto& fn = program.top;
    fn.blocks[0].stmts.push_back(assign(2, var(0, intType(8))));
    fn.blocks[0].stmts.push_back(addOp(2, var(2, intType(8)), lit(1, intType(8)), 8));

    auto debug = lowerDebug(program);
    CHECK(debug.find("s10predicate") != std::string::npos);
    CHECK(debug.find("guard=0x1<bool> assign out_v1 = a_v0<u8>") != std::string::npos);
    CHECK(debug.find("guard=0x1<bool> op out_v2 = Add<8>(out_v1<u8>, 0x1<u8>)") != std::string::npos);
    CHECK(debug.find("term ") == std::string::npos);
    CHECK(debug.find(" phi ") == std::string::npos);
}

static void branchPhiLowersToMux() {
    auto debug = lowerDebug(branchProgram());
    CHECK(debug.find("block_guards") != std::string::npos);
    CHECK(debug.find("bb1 guard=cond_v0<bool>") != std::string::npos);
    CHECK(debug.find("LogicalNot<1>(cond_v0<bool>)") != std::string::npos);
    CHECK(debug.find("BoolOr<1>(") != std::string::npos);
    CHECK(debug.find("op out_v3 = Mux<8>(") != std::string::npos);
    CHECK(debug.find("lowered_phi") != std::string::npos);
    CHECK(debug.find("out initial=out_v0 final=out_v3") != std::string::npos);
}

static void lookupIsPreserved() {
    auto program = baseProgram();
    auto& fn = program.top;
    fn.blocks[0].stmts.push_back(lookupStmt(4, var(3, intType(2)),
                                            {lit(3, intType(8)), lit(4, intType(8))}));
    fn.blocks[0].stmts.push_back(assign(2, var(4, intType(8))));

    auto debug = lowerDebug(program);
    CHECK(debug.find("lookup tmp_v0 = lookup(idx_v0<u2>, 0x3<u8>, 0x4<u8>)") != std::string::npos);
    CHECK(debug.find("out initial=out_v0 final=out_v1") != std::string::npos);
}

static s8opnorm::S8NormProgram switchProgram() {
    auto program = baseProgram();
    auto& fn = program.top;
    fn.exit = 4;
    fn.blocks.clear();
    fn.blocks.resize(5);
    for (int i = 0; i < 5; ++i) fn.blocks[static_cast<std::size_t>(i)].id = i;
    s8opnorm::S8Terminator term;
    term.kind = s8opnorm::S8TermKind::Switch;
    term.switch_value = var(3, intType(2));
    term.switch_targets.push_back(s8opnorm::S8SwitchTarget{lit(0, intType(2)), 1});
    term.switch_targets.push_back(s8opnorm::S8SwitchTarget{lit(1, intType(2)), 2});
    term.default_target = 3;
    fn.blocks[0].terminator = std::move(term);
    fn.blocks[1].stmts.push_back(assign(2, lit(10, intType(8))));
    fn.blocks[1].terminator = jump(4);
    fn.blocks[2].stmts.push_back(assign(2, lit(11, intType(8))));
    fn.blocks[2].terminator = jump(4);
    fn.blocks[3].stmts.push_back(assign(2, lit(12, intType(8))));
    fn.blocks[3].terminator = jump(4);
    fn.blocks[4].terminator = exitTerm();
    return program;
}

static void switchBuildsCaseAndDefaultGuards() {
    auto debug = lowerDebug(switchProgram());
    CHECK(debug.find("guard_eq") != std::string::npos);
    CHECK(debug.find("Eq<1>(idx_v0<u2>, 0x0<u2>)") != std::string::npos);
    CHECK(debug.find("Eq<1>(idx_v0<u2>, 0x1<u2>)") != std::string::npos);
    CHECK(debug.find("BoolOr<1>(") != std::string::npos);
    CHECK(debug.find("LogicalNot<1>(") != std::string::npos);
    CHECK(debug.find("lowered_phi") != std::string::npos);
}

static void sourcePipelineRunsThroughS10() {
    auto debug = runSourceToS10("testv2/fixtures/s9ssa/source_ssa.logic.cpp");
    CHECK(debug.find("s10predicate") != std::string::npos);
    CHECK(debug.find("lowered_lookupwrite") != std::string::npos);
    CHECK(debug.find("lookup ") != std::string::npos);
    CHECK(debug.find("term ") == std::string::npos);
    CHECK(debug.find(" phi ") == std::string::npos);
}

static s10predicate::S10Operand predValue(s10predicate::S10ValueId value,
                                          s10predicate::S10Type type) {
    s10predicate::S10Operand operand;
    operand.kind = s10predicate::S10OperandKind::Value;
    operand.value = value;
    operand.type = type;
    return operand;
}

static s10predicate::S10Operand predLiteral(std::uint64_t value,
                                            s10predicate::S10Type type) {
    s10predicate::S10Operand operand;
    operand.kind = s10predicate::S10OperandKind::Literal;
    operand.type = type;
    operand.literal = literalBits(value, type);
    return operand;
}

static s10predicate::S10Value predS10Value(s10predicate::S10ValueId id,
                                           s10predicate::SymbolId base,
                                           int version,
                                           s10predicate::S10Type type,
                                           s10predicate::S10ValueKind kind,
                                           std::string name) {
    s10predicate::S10Value value;
    value.id = id;
    value.base_symbol = base;
    value.version = version;
    value.type = type;
    value.kind = kind;
    value.debug_name = std::move(name);
    return value;
}

static void readonlyCheckRejectsGuardedReadOutsideCoverage() {
    s10predicate::S10PredicateProgram program;
    program.name = "bad";
    program.base_symbols.push_back(s10predicate::S10Symbol{0, boolType(), "cond", s10predicate::S10SymbolRole::Port});
    program.base_symbols.push_back(s10predicate::S10Symbol{1, intType(8), "out", s10predicate::S10SymbolRole::Port});
    program.base_symbols.push_back(s10predicate::S10Symbol{2, intType(8), "x", s10predicate::S10SymbolRole::Local});
    program.values.push_back(predS10Value(0, 0, 0, boolType(), s10predicate::S10ValueKind::Initial, "cond"));
    program.values.push_back(predS10Value(1, 2, 0, intType(8), s10predicate::S10ValueKind::Statement, "x"));
    program.values.push_back(predS10Value(2, 1, 0, intType(8), s10predicate::S10ValueKind::Statement, "out"));

    s10predicate::S10Definition define_x;
    define_x.kind = s10predicate::S10DefKind::Assign;
    define_x.target = 1;
    define_x.guard = predValue(0, boolType());
    define_x.value = predLiteral(7, intType(8));
    program.definitions.push_back(std::move(define_x));

    s10predicate::S10Definition define_out;
    define_out.kind = s10predicate::S10DefKind::Assign;
    define_out.target = 2;
    define_out.guard = predLiteral(1, boolType());
    define_out.value = predValue(1, intType(8));
    program.definitions.push_back(std::move(define_out));

    program.ports.push_back(s10predicate::S10Port{0, ParamDirection::Input, ParamPassingKind::Value, 0, std::nullopt, std::nullopt});
    program.ports.push_back(s10predicate::S10Port{1, ParamDirection::Output, ParamPassingKind::MutableRef, std::nullopt, 2, predLiteral(1, boolType())});

    bool failed = false;
    try {
        s10predicate::verifyPredicateProgram(program);
    } catch (const RTLZZException& ex) {
        failed = true;
        CHECK(std::string(ex.what()).find("guarded read outside value definition coverage") != std::string::npos);
    }
    CHECK(failed);
}

int main() {
    straightLineUsesTrueGuard();
    branchPhiLowersToMux();
    lookupIsPreserved();
    switchBuildsCaseAndDefaultGuards();
    sourcePipelineRunsThroughS10();
    readonlyCheckRejectsGuardedReadOutsideCoverage();
    return 0;
}
