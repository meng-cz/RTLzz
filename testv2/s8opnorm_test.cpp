#include "ast/ASTBuilder.h"
#include "s1apinorm/S1APINorm.h"
#include "s2validate/S2Validate.h"
#include "s3statementize/S3Statementize.h"
#include "s4cfg/S4CFG.h"
#include "s5unroll/S5Unroll.h"
#include "s6inline/S6Inline.h"
#include "s7flatten/S7Flatten.h"
#include "s8opnorm/S8Norm.h"

#include <cstdlib>
#include <iostream>
#include <string>
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
    if (!build.error.empty()) std::cerr << build.error << "\n";
    CHECK(build.error.empty());
    CHECK(build.function.has_value());
    return std::move(build.function.value());
}

static TypeInfo intType(int width) {
    return make_hw_type("Int", width, true);
}

static TypeInfo uintType(int width) {
    return make_hw_type("UInt", width, false);
}

static TypeInfo boolType() {
    return canonical_bool_type();
}

static s7flatten::S7Symbol symbol(s7flatten::SymbolId id,
                                  const std::string& name,
                                  TypeInfo type,
                                  s7flatten::S7SymbolRole role = s7flatten::S7SymbolRole::Local) {
    s7flatten::S7Symbol out;
    out.id = id;
    out.debug_name = name;
    out.type = std::move(type);
    out.role = role;
    return out;
}

static s7flatten::S7Operand var(const s7flatten::S7Symbol& symbol, TypeInfo use_type = {}) {
    s7flatten::S7Operand out;
    out.kind = s7flatten::S7OperandKind::Var;
    out.symbol = symbol.id;
    out.type = use_type.width > 0 || use_type.name == "bool" || use_type.hw_kind == "bool"
        ? std::move(use_type)
        : symbol.type;
    return out;
}

static s7flatten::S7Operand literal(const std::string& text, TypeInfo type) {
    s7flatten::S7Operand out;
    out.kind = s7flatten::S7OperandKind::Literal;
    out.literal_value = text;
    out.type = std::move(type);
    return out;
}

static s7flatten::S7Stmt opStmt(s7flatten::SymbolId target,
                                s7flatten::S7Operation op) {
    s7flatten::S7Stmt stmt;
    stmt.kind = s7flatten::S7StmtKind::Op;
    stmt.target = target;
    stmt.op = std::move(op);
    return stmt;
}

static s7flatten::S7Stmt assignStmt(s7flatten::SymbolId target,
                                    s7flatten::S7Operand value) {
    s7flatten::S7Stmt stmt;
    stmt.kind = s7flatten::S7StmtKind::Assign;
    stmt.target = target;
    stmt.value = std::move(value);
    return stmt;
}

static s7flatten::S7FlattenedProgram baseProgram() {
    s7flatten::S7FlattenedProgram program;
    auto& fn = program.top;
    fn.name = "hls_main";
    fn.entry = 0;
    fn.exit = 0;
    fn.symbols.push_back(symbol(0, "a", uintType(8), s7flatten::S7SymbolRole::Port));
    fn.symbols.push_back(symbol(1, "b", uintType(4), s7flatten::S7SymbolRole::Port));
    fn.symbols.push_back(symbol(2, "s", intType(8), s7flatten::S7SymbolRole::Port));
    fn.symbols.push_back(symbol(3, "cond", boolType(), s7flatten::S7SymbolRole::Port));
    fn.symbols.push_back(symbol(4, "out8", uintType(8), s7flatten::S7SymbolRole::Port));
    fn.symbols.push_back(symbol(5, "out12", uintType(12), s7flatten::S7SymbolRole::Port));
    fn.ports.push_back(s7flatten::S7Port{0, ParamDirection::Input, ParamPassingKind::Value});
    fn.ports.push_back(s7flatten::S7Port{1, ParamDirection::Input, ParamPassingKind::Value});
    fn.ports.push_back(s7flatten::S7Port{2, ParamDirection::Input, ParamPassingKind::Value});
    fn.ports.push_back(s7flatten::S7Port{3, ParamDirection::Input, ParamPassingKind::Value});
    fn.ports.push_back(s7flatten::S7Port{4, ParamDirection::Output, ParamPassingKind::MutableRef});
    fn.ports.push_back(s7flatten::S7Port{5, ParamDirection::Output, ParamPassingKind::MutableRef});
    s7flatten::S7BasicBlock block;
    block.id = 0;
    block.terminator.kind = s7flatten::S7TermKind::Exit;
    fn.blocks.push_back(std::move(block));
    return program;
}

static std::string normalizeDebug(s7flatten::S7FlattenedProgram program) {
    s8opnorm::NormOptions options;
    options.debug_print = true;
    auto result = s8opnorm::normalizeOperations(program, options);
    if (!result.ok()) std::cerr << result.error->formatted << "\n";
    CHECK(result.ok());
    CHECK(result.program.has_value());
    s8opnorm::verifyNormProgram(result.program.value());
    CHECK(!result.debug_text.empty());
    return result.debug_text;
}

static std::string runSourceToS8(const std::string& file) {
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
    s8opnorm::NormOptions options;
    options.debug_print = true;
    auto s8 = s8opnorm::normalizeOperations(s7.program.value(), options);
    if (!s8.ok()) std::cerr << s8.error->formatted << "\n";
    CHECK(s8.ok());
    CHECK(s8.program.has_value());
    s8opnorm::verifyNormProgram(s8.program.value());
    return s8.debug_text;
}

static void addUsesFixintWidthThenCastsToTarget() {
    auto program = baseProgram();
    auto& fn = program.top;
    s7flatten::S7Operation add;
    add.kind = s7flatten::S7OpKind::Binary;
    add.binary_op = s7flatten::S7BinaryOp::Add;
    add.operands = {var(fn.symbols[0]), var(fn.symbols[1])};
    fn.blocks[0].stmts.push_back(opStmt(4, std::move(add)));

    auto debug = normalizeDebug(std::move(program));
    CHECK(debug.find("ZExt<9>(a<u8>)") != std::string::npos);
    CHECK(debug.find("ZExt<9>(b<u4>)") != std::string::npos);
    CHECK(debug.find("Add<9>(") != std::string::npos);
    CHECK(debug.find("op out8 = Trunc<8>(__s8_norm_add_") != std::string::npos);
}

static void multiplyUsesOperandWidthSum() {
    auto program = baseProgram();
    auto& fn = program.top;
    s7flatten::S7Operation mul;
    mul.kind = s7flatten::S7OpKind::Binary;
    mul.binary_op = s7flatten::S7BinaryOp::Mul;
    mul.operands = {var(fn.symbols[0]), var(fn.symbols[1])};
    fn.blocks[0].stmts.push_back(opStmt(5, std::move(mul)));

    auto debug = normalizeDebug(std::move(program));
    CHECK(debug.find("Mul<12>(a<u8>, b<u4>)") != std::string::npos);
    CHECK(debug.find("assign out12 = __s8_norm_mul_") != std::string::npos);
}

static void signedRightShiftUsesOperandSignedView() {
    auto program = baseProgram();
    auto& fn = program.top;
    s7flatten::S7Operation shr;
    shr.kind = s7flatten::S7OpKind::Binary;
    shr.binary_op = s7flatten::S7BinaryOp::Shr;
    shr.operands = {var(fn.symbols[2]), literal("1", uintType(4))};
    fn.blocks[0].stmts.push_back(opStmt(4, std::move(shr)));

    auto debug = normalizeDebug(std::move(program));
    CHECK(debug.find("AShr<8>(s:s<u8>, 0x1<u4>)") != std::string::npos);
}

static void muxCastsConditionAndArmsToTarget() {
    auto program = baseProgram();
    auto& fn = program.top;
    s7flatten::S7Operation mux;
    mux.kind = s7flatten::S7OpKind::Ternary;
    mux.operands = {var(fn.symbols[0]), var(fn.symbols[1]), literal("0x7", uintType(4))};
    fn.blocks[0].stmts.push_back(opStmt(4, std::move(mux)));

    auto debug = normalizeDebug(std::move(program));
    CHECK(debug.find("ReduceOr<1>(a<u8>)") != std::string::npos);
    CHECK(debug.find("Mux<8>(") != std::string::npos);
    CHECK(debug.find("ZExt<8>(b<u4>)") != std::string::npos);
}

static void lookupElementsCastToTarget() {
    auto program = baseProgram();
    auto& fn = program.top;
    s7flatten::S7Stmt lookup;
    lookup.kind = s7flatten::S7StmtKind::Lookup;
    lookup.target = 4;
    lookup.lookup_index = var(fn.symbols[1]);
    lookup.lookup_elements = {literal("1", uintType(4)), literal("2", uintType(4))};
    fn.blocks[0].stmts.push_back(std::move(lookup));

    auto debug = normalizeDebug(std::move(program));
    CHECK(debug.find("ZExt<8>(0x1<u4>)") != std::string::npos);
    CHECK(debug.find("lookup out8 = lookup(b<u4>, __s8_norm_cast_") != std::string::npos);
}

static void nonConstantDivAndModAreRejected() {
    auto program = baseProgram();
    auto& fn = program.top;
    s7flatten::S7Operation div;
    div.kind = s7flatten::S7OpKind::Binary;
    div.binary_op = s7flatten::S7BinaryOp::Div;
    div.operands = {var(fn.symbols[0]), var(fn.symbols[1])};
    fn.blocks[0].stmts.push_back(opStmt(4, std::move(div)));
    auto result = s8opnorm::normalizeOperations(program);
    CHECK(!result.ok());
    CHECK(result.error->message.find("second operand is a constant") != std::string::npos);
}

static void powerOfTwoDivAndModUseSlice() {
    auto div_program = baseProgram();
    auto& div_fn = div_program.top;
    s7flatten::S7Operation div;
    div.kind = s7flatten::S7OpKind::Binary;
    div.binary_op = s7flatten::S7BinaryOp::Div;
    div.operands = {var(div_fn.symbols[0]), literal("8", uintType(8))};
    div_fn.blocks[0].stmts.push_back(opStmt(4, std::move(div)));
    auto div_debug = normalizeDebug(std::move(div_program));
    CHECK(div_debug.find("Slice<5>(a<u8>) meta{hi=7,lo=3") != std::string::npos);
    CHECK(div_debug.find("ZExt<8>(__s8_norm_divpow2_") != std::string::npos);

    auto mod_program = baseProgram();
    auto& mod_fn = mod_program.top;
    s7flatten::S7Operation mod;
    mod.kind = s7flatten::S7OpKind::Binary;
    mod.binary_op = s7flatten::S7BinaryOp::Mod;
    mod.operands = {var(mod_fn.symbols[0]), literal("8", uintType(8))};
    mod_fn.blocks[0].stmts.push_back(opStmt(4, std::move(mod)));
    auto mod_debug = normalizeDebug(std::move(mod_program));
    CHECK(mod_debug.find("Slice<3>(a<u8>) meta{hi=2,lo=0") != std::string::npos);
    CHECK(mod_debug.find("ZExt<8>(__s8_norm_modpow2_") != std::string::npos);
}

static void nonPowerOfTwoDivAndModUseMagicMultiply() {
    auto div_program = baseProgram();
    auto& div_fn = div_program.top;
    s7flatten::S7Operation div;
    div.kind = s7flatten::S7OpKind::Binary;
    div.binary_op = s7flatten::S7BinaryOp::Div;
    div.operands = {var(div_fn.symbols[0]), literal("10", uintType(8))};
    div_fn.blocks[0].stmts.push_back(opStmt(4, std::move(div)));
    auto div_debug = normalizeDebug(std::move(div_program));
    CHECK(div_debug.find("Mul<16>(a<u8>") != std::string::npos);
    CHECK(div_debug.find("Slice<8>(__s8_norm_divmul_") != std::string::npos);
    CHECK(div_debug.find("LShr<8>(") != std::string::npos);

    auto mod_program = baseProgram();
    auto& mod_fn = mod_program.top;
    s7flatten::S7Operation mod;
    mod.kind = s7flatten::S7OpKind::Binary;
    mod.binary_op = s7flatten::S7BinaryOp::Mod;
    mod.operands = {var(mod_fn.symbols[0]), literal("10", uintType(8))};
    mod_fn.blocks[0].stmts.push_back(opStmt(4, std::move(mod)));
    auto mod_debug = normalizeDebug(std::move(mod_program));
    CHECK(mod_debug.find("Mul<16>(a<u8>") != std::string::npos);
    CHECK(mod_debug.find("Mul<16>(__s8_norm_") != std::string::npos);
    CHECK(mod_debug.find("Sub<8>(a<u8>") != std::string::npos);
}

static void nonPowerOfTwoDivisionUsesCorrectionWhenNeeded() {
    auto program = baseProgram();
    auto& fn = program.top;
    s7flatten::S7Operation div;
    div.kind = s7flatten::S7OpKind::Binary;
    div.binary_op = s7flatten::S7BinaryOp::Div;
    div.operands = {var(fn.symbols[0]), literal("7", uintType(8))};
    fn.blocks[0].stmts.push_back(opStmt(4, std::move(div)));
    auto debug = normalizeDebug(std::move(program));
    CHECK(debug.find("__s8_norm_divcorr_sub_") != std::string::npos);
    CHECK(debug.find("__s8_norm_divcorr_add_") != std::string::npos);
    CHECK(debug.find("__s8_norm_divcorr_shift_") != std::string::npos);
}

static void divisorAboveInputRangeSimplifies() {
    auto div_program = baseProgram();
    auto& div_fn = div_program.top;
    s7flatten::S7Operation div;
    div.kind = s7flatten::S7OpKind::Binary;
    div.binary_op = s7flatten::S7BinaryOp::Div;
    div.operands = {var(div_fn.symbols[0]), literal("300", uintType(16))};
    div_fn.blocks[0].stmts.push_back(opStmt(4, std::move(div)));
    auto div_debug = normalizeDebug(std::move(div_program));
    CHECK(div_debug.find("assign out8 = 0x0<u8>") != std::string::npos);

    auto mod_program = baseProgram();
    auto& mod_fn = mod_program.top;
    s7flatten::S7Operation mod;
    mod.kind = s7flatten::S7OpKind::Binary;
    mod.binary_op = s7flatten::S7BinaryOp::Mod;
    mod.operands = {var(mod_fn.symbols[0]), literal("300", uintType(16))};
    mod_fn.blocks[0].stmts.push_back(opStmt(4, std::move(mod)));
    auto mod_debug = normalizeDebug(std::move(mod_program));
    CHECK(mod_debug.find("assign out8 = a<u8>") != std::string::npos);
}

static void signedConstantDivAndModUseAbsAndRestoreSign() {
    auto program = baseProgram();
    auto& fn = program.top;
    s7flatten::S7Operation div;
    div.kind = s7flatten::S7OpKind::Binary;
    div.binary_op = s7flatten::S7BinaryOp::Div;
    div.operands = {var(fn.symbols[2]), literal("3", uintType(8))};
    fn.blocks[0].stmts.push_back(opStmt(4, std::move(div)));
    auto div_debug = normalizeDebug(std::move(program));
    CHECK(div_debug.find("__s8_norm_signbit_") != std::string::npos);
    CHECK(div_debug.find("__s8_norm_abs_") != std::string::npos);
    CHECK(div_debug.find("__s8_norm_sdiv_neg_") != std::string::npos);
    CHECK(div_debug.find("__s8_norm_sdiv_mux_") != std::string::npos);

    auto mod_program = baseProgram();
    auto& mod_fn = mod_program.top;
    s7flatten::S7Operation mod;
    mod.kind = s7flatten::S7OpKind::Binary;
    mod.binary_op = s7flatten::S7BinaryOp::Mod;
    mod.operands = {var(mod_fn.symbols[2]), literal("3", uintType(8))};
    mod_fn.blocks[0].stmts.push_back(opStmt(4, std::move(mod)));
    auto mod_debug = normalizeDebug(std::move(mod_program));
    CHECK(mod_debug.find("__s8_norm_smod_neg_") != std::string::npos);
    CHECK(mod_debug.find("__s8_norm_smod_mux_") != std::string::npos);
}

static void negativeConstantDivisorFlipsQuotientSign() {
    auto program = baseProgram();
    auto& fn = program.top;
    s7flatten::S7Operation div;
    div.kind = s7flatten::S7OpKind::Binary;
    div.binary_op = s7flatten::S7BinaryOp::Div;
    div.operands = {var(fn.symbols[2]), literal("-3", intType(8))};
    fn.blocks[0].stmts.push_back(opStmt(4, std::move(div)));
    auto debug = normalizeDebug(std::move(program));
    CHECK(debug.find("__s8_norm_boolnot_") != std::string::npos);
    CHECK(debug.find("__s8_norm_sdiv_mux_") != std::string::npos);

    auto mod_program = baseProgram();
    auto& mod_fn = mod_program.top;
    s7flatten::S7Operation mod;
    mod.kind = s7flatten::S7OpKind::Binary;
    mod.binary_op = s7flatten::S7BinaryOp::Mod;
    mod.operands = {var(mod_fn.symbols[2]), literal("-3", intType(8))};
    mod_fn.blocks[0].stmts.push_back(opStmt(4, std::move(mod)));
    auto mod_debug = normalizeDebug(std::move(mod_program));
    CHECK(mod_debug.find("__s8_norm_boolnot_") == std::string::npos);
    CHECK(mod_debug.find("__s8_norm_smod_mux_") != std::string::npos);
}

static void sourcePipelineRunsThroughS8() {
    auto debug = runSourceToS8("testv2/fixtures/s7flatten/source_flatten.logic.cpp");
    CHECK(debug.find("s8opnorm") != std::string::npos);
    CHECK(debug.find("Add<9>(") != std::string::npos);
    CHECK(debug.find("lookup ") != std::string::npos);
    CHECK(debug.find("lookupwrite [arr__idx_0") != std::string::npos);
}

int main() {
    addUsesFixintWidthThenCastsToTarget();
    multiplyUsesOperandWidthSum();
    signedRightShiftUsesOperandSignedView();
    muxCastsConditionAndArmsToTarget();
    lookupElementsCastToTarget();
    nonConstantDivAndModAreRejected();
    powerOfTwoDivAndModUseSlice();
    nonPowerOfTwoDivAndModUseMagicMultiply();
    nonPowerOfTwoDivisionUsesCorrectionWhenNeeded();
    divisorAboveInputRangeSimplifies();
    signedConstantDivAndModUseAbsAndRestoreSign();
    negativeConstantDivisorFlipsQuotientSign();
    sourcePipelineRunsThroughS8();
    return 0;
}
