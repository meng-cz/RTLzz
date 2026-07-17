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

#include <cstdlib>
#include <cstdint>
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
    fn.symbols.push_back(symbol(3, "x", intType(8), s8opnorm::S8SymbolRole::Local));
    fn.ports.push_back(s8opnorm::S8Port{0, ParamDirection::Input, ParamPassingKind::Value});
    fn.ports.push_back(s8opnorm::S8Port{1, ParamDirection::Input, ParamPassingKind::Value});
    fn.ports.push_back(s8opnorm::S8Port{2, ParamDirection::Output, ParamPassingKind::MutableRef});
    s8opnorm::S8BasicBlock block;
    block.id = 0;
    block.terminator = exitTerm();
    fn.blocks.push_back(std::move(block));
    return program;
}

static std::string buildDebug(const s8opnorm::S8NormProgram& program) {
    s9ssa::SSAOptions options;
    options.debug_print = true;
    auto result = s9ssa::buildSSA(program, options);
    if (!result.ok()) std::cerr << result.error->formatted << "\n";
    CHECK(result.ok());
    CHECK(result.program.has_value());
    s9ssa::verifySSAProgram(result.program.value());
    CHECK(!result.debug_text.empty());
    return result.debug_text;
}

static std::string runSourceToS9(const std::string& file) {
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
    s9ssa::SSAOptions options;
    options.debug_print = true;
    auto s9 = s9ssa::buildSSA(s8.program.value(), options);
    if (!s9.ok()) std::cerr << s9.error->formatted << "\n";
    CHECK(s9.ok());
    CHECK(s9.program.has_value());
    s9ssa::verifySSAProgram(s9.program.value());
    return s9.debug_text;
}

static void straightLineVersionsIncrement() {
    auto program = baseProgram();
    auto& fn = program.top;
    fn.blocks[0].stmts.push_back(assign(2, var(0, intType(8))));
    fn.blocks[0].stmts.push_back(addOp(2, var(2, intType(8)), lit(1, intType(8)), 8));

    auto debug = buildDebug(program);
    CHECK(debug.find("assign out_v1 = a_v0<u8>") != std::string::npos);
    CHECK(debug.find("op out_v2 = Add<8>(out_v1<u8>, 0x1<u8>)") != std::string::npos);
    CHECK(debug.find("out initial=out_v0 final=out_v2") != std::string::npos);
}

static s8opnorm::S8NormProgram branchProgram(bool else_writes, bool use_local_after_merge) {
    auto program = baseProgram();
    auto& fn = program.top;
    fn.exit = 3;
    fn.blocks.clear();
    fn.blocks.resize(4);
    for (int i = 0; i < 4; ++i) fn.blocks[static_cast<std::size_t>(i)].id = i;
    fn.blocks[0].terminator = branch(var(1, boolType()), 1, 2);
    fn.blocks[1].stmts.push_back(assign(use_local_after_merge ? 3 : 2, lit(1, intType(8))));
    fn.blocks[1].terminator = jump(3);
    if (else_writes) {
        fn.blocks[2].stmts.push_back(assign(use_local_after_merge ? 3 : 2, lit(2, intType(8))));
    }
    fn.blocks[2].terminator = jump(3);
    if (use_local_after_merge) {
        fn.blocks[3].stmts.push_back(assign(2, var(3, intType(8))));
    }
    fn.blocks[3].terminator = exitTerm();
    return program;
}

static void ifElseCreatesPhi() {
    auto debug = buildDebug(branchProgram(true, false));
    CHECK(debug.find("bb3") != std::string::npos);
    CHECK(debug.find("phi out_v3 = phi(") != std::string::npos);
    CHECK(debug.find("bb1:out_v2") != std::string::npos);
    CHECK(debug.find("bb2:out_v1") != std::string::npos);
    CHECK(debug.find("out initial=out_v0 final=out_v3") != std::string::npos);
}

static void oneBranchOutputWriteMergesWithInitialValue() {
    auto debug = buildDebug(branchProgram(false, false));
    CHECK(debug.find("phi out_v2 = phi(bb1:out_v1, bb2:out_v0)") != std::string::npos);
    CHECK(debug.find("out initial=out_v0 final=out_v2") != std::string::npos);
}

static void localMissingIncomingIsRejected() {
    auto program = branchProgram(false, true);
    auto result = s9ssa::buildSSA(program);
    CHECK(!result.ok());
    CHECK(result.error->message.find("Missing incoming SSA value") != std::string::npos);
}

static void lookupWriteLowersToElementMuxes() {
    s8opnorm::S8NormProgram program;
    auto& fn = program.top;
    fn.name = "hls_main";
    fn.entry = 0;
    fn.exit = 0;
    fn.symbols.push_back(symbol(0, "idx", intType(2), s8opnorm::S8SymbolRole::Port));
    fn.symbols.push_back(symbol(1, "arr0", intType(4), s8opnorm::S8SymbolRole::Port));
    fn.symbols.push_back(symbol(2, "arr1", intType(4), s8opnorm::S8SymbolRole::Port));
    fn.ports.push_back(s8opnorm::S8Port{0, ParamDirection::Input, ParamPassingKind::Value});
    fn.ports.push_back(s8opnorm::S8Port{1, ParamDirection::Output, ParamPassingKind::MutableRef});
    fn.ports.push_back(s8opnorm::S8Port{2, ParamDirection::Output, ParamPassingKind::MutableRef});

    s8opnorm::S8Stmt lookupwrite;
    lookupwrite.kind = s8opnorm::S8StmtKind::LookupWrite;
    lookupwrite.lookup_index = var(0, intType(2));
    lookupwrite.lookup_value = lit(7, intType(8));
    lookupwrite.lookup_elements = {var(1, intType(4)), var(2, intType(4))};
    lookupwrite.lookup_write_targets = {1, 2};

    s8opnorm::S8BasicBlock block;
    block.id = 0;
    block.stmts.push_back(std::move(lookupwrite));
    block.terminator = exitTerm();
    fn.blocks.push_back(std::move(block));

    auto debug = buildDebug(program);
    CHECK(debug.find(" = lookupwrite(") == std::string::npos);
    CHECK(debug.find("lowered_lookupwrite") != std::string::npos);
    CHECK(debug.find("Eq<1>(idx_v0<u2>, 0x0<u2>)") != std::string::npos);
    CHECK(debug.find("Eq<1>(idx_v0<u2>, 0x1<u2>)") != std::string::npos);
    CHECK(debug.find("Mux<4>(") != std::string::npos);
    CHECK(debug.find("Trunc<4>(0x7<u8>)") != std::string::npos);
}

static void sourcePipelineRunsThroughS9() {
    auto debug = runSourceToS9("testv2/fixtures/s9ssa/source_ssa.logic.cpp");
    CHECK(debug.find("s9ssa") != std::string::npos);
    CHECK(debug.find("out initial=out_v0") != std::string::npos);
    CHECK(debug.find("lowered_lookupwrite") != std::string::npos);
    CHECK(debug.find(" = lookupwrite(") == std::string::npos);
}

int main() {
    straightLineVersionsIncrement();
    ifElseCreatesPhi();
    oneBranchOutputWriteMergesWithInitialValue();
    localMissingIncomingIsRejected();
    lookupWriteLowersToElementMuxes();
    sourcePipelineRunsThroughS9();
    return 0;
}
