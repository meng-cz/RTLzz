#include "s0ast/S0AST.h"
#include "backend/beir.hpp"
#include "backend/rtlgen.hpp"
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
#include "s11beir/S11BEIR.h"

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
    if (type.width >= 64) {
        literal.words.push_back(value);
    } else {
        literal.words.push_back(value & ((std::uint64_t{1} << type.width) - 1));
    }
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

static s8opnorm::S8Stmt opStmt(s8opnorm::SymbolId target,
                               s8opnorm::S8OpKind kind,
                               s8opnorm::S8Type result_type,
                               std::vector<s8opnorm::S8Operand> operands) {
    s8opnorm::S8Stmt stmt;
    stmt.kind = s8opnorm::S8StmtKind::Op;
    stmt.target = target;
    stmt.op.kind = kind;
    stmt.op.result_width = result_type.width;
    stmt.op.operands = std::move(operands);
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

static s8opnorm::S8Terminator exitTerm() {
    s8opnorm::S8Terminator term;
    term.kind = s8opnorm::S8TermKind::Exit;
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

static s10predicate::S10PredicateProgram lowerToS10(const s8opnorm::S8NormProgram& program) {
    auto s9 = s9ssa::buildSSA(program);
    if (!s9.ok()) std::cerr << s9.error->formatted << "\n";
    CHECK(s9.ok());
    CHECK(s9.program.has_value());
    auto s10 = s10predicate::lowerPredicates(s9.program.value());
    if (!s10.ok()) std::cerr << s10.error->formatted << "\n";
    CHECK(s10.ok());
    CHECK(s10.program.has_value());
    return std::move(s10.program.value());
}

static beir::Program lowerToBEIR(const s8opnorm::S8NormProgram& program,
                                 bool debug_print = false) {
    auto s10 = lowerToS10(program);
    s11beir::BEIROptions options;
    options.debug_print = debug_print;
    auto s11 = s11beir::buildBEIR(s10, options);
    if (!s11.ok()) std::cerr << s11.error->formatted << "\n";
    CHECK(s11.ok());
    CHECK(s11.program.has_value());
    if (debug_print) CHECK(!s11.debug_text.empty());
    return std::move(s11.program.value());
}

static beir::Program runSourceToBEIR(const std::string& file) {
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
    return lowerToBEIR(s8.program.value(), true);
}

static int countKind(const beir::Program& program, beir::OperationKind kind) {
    int count = 0;
    for (const auto& signal : program.signals) {
        if (signal.driver && signal.driver->kind == kind) ++count;
    }
    return count;
}

static const beir::Signal* findSignal(const beir::Program& program,
                                      const std::string& name) {
    for (const auto& signal : program.signals) {
        if (signal.name == name) return &signal;
    }
    return nullptr;
}

static const beir::Port* findPort(const beir::Program& program,
                                  const std::string& name) {
    for (const auto& port : program.ports) {
        if (port.name == name) return &port;
    }
    return nullptr;
}

static beir::ValueType beirType(int width, std::vector<int> array_dims = {}) {
    beir::ValueType type;
    type.width = width;
    type.array_dims = std::move(array_dims);
    return type;
}

static beir::Operand beirPortOperand(std::string name, beir::ValueType type) {
    beir::Operand operand;
    operand.kind = beir::OperandKind::Port;
    operand.text = std::move(name);
    operand.type = std::move(type);
    return operand;
}

static beir::Signal beirSignal(beir::NodeId id,
                               std::string name,
                               beir::ValueType type,
                               std::string port_name = {},
                               int port_element_index = -1) {
    beir::Signal signal;
    signal.id = id;
    signal.name = std::move(name);
    signal.type = std::move(type);
    signal.port_name = std::move(port_name);
    signal.port_element_index = port_element_index;
    return signal;
}

static void rtlgenConnectsScalarPortElementsWithoutArraySelect() {
    beir::Program program;
    program.function_name = "scalar_port_bindings";
    beir::ValueType scalar = beirType(8);

    auto in_signal = beirSignal(0, "in_internal", scalar, "in", 0);
    beir::Operation in_read;
    in_read.kind = beir::OperationKind::PortRead;
    in_read.type = scalar;
    in_read.operands.push_back(beirPortOperand("in", scalar));
    in_signal.driver = std::move(in_read);
    program.signals.push_back(std::move(in_signal));

    program.signals.push_back(beirSignal(1, "out_internal", scalar, "out", 0));
    program.signals.push_back(beirSignal(2, "arr_0", scalar, "arr", 0));
    program.signals.push_back(beirSignal(3, "arr_1", scalar, "arr", 1));

    beir::Port in;
    in.name = "in";
    in.direction = beir::PortDirection::Input;
    in.type = scalar;
    in.element_nodes.push_back(0);
    program.ports.push_back(std::move(in));

    beir::Port out;
    out.name = "out";
    out.direction = beir::PortDirection::Output;
    out.type = scalar;
    out.element_nodes.push_back(1);
    program.ports.push_back(std::move(out));

    beir::Port arr;
    arr.name = "arr";
    arr.direction = beir::PortDirection::Output;
    arr.type = beirType(8, {2});
    arr.element_nodes.push_back(2);
    arr.element_nodes.push_back(3);
    program.ports.push_back(std::move(arr));

    std::string rtl = rtlgen::emitSystemVerilog(program);
    CHECK(rtl.find("assign in_internal = in;") != std::string::npos);
    CHECK(rtl.find("assign in_internal = in[0];") == std::string::npos);
    CHECK(rtl.find("assign out = out_internal;") != std::string::npos);
    CHECK(rtl.find("assign out[0] = out_internal;") == std::string::npos);
    CHECK(rtl.find("assign arr[0] = arr_0;") != std::string::npos);
    CHECK(rtl.find("assign arr[1] = arr_1;") != std::string::npos);
}

static void straightLineBuildsPortsAndOutputAssign() {
    auto program = baseProgram();
    auto& block = program.top.blocks[0];
    block.stmts.push_back(assign(2, var(0, intType(8))));
    block.stmts.push_back(opStmt(2, s8opnorm::S8OpKind::BitXor, intType(8),
                                 {var(2, intType(8)), lit(1, intType(8))}));

    auto beir_program = lowerToBEIR(program, true);
    CHECK(beir_program.function_name == "hls_main");
    CHECK(beir_program.inputs.size() == 3);
    CHECK(beir_program.outputs.size() == 1);
    CHECK(beir_program.outputs[0] == "out");
    const beir::Signal* out = findSignal(beir_program, "out");
    CHECK(out != nullptr);
    CHECK(out->port_name == "out");
    CHECK(out->driver.has_value());
    CHECK(out->driver->kind == beir::OperationKind::Assign);
    CHECK(countKind(beir_program, beir::OperationKind::PortRead) == 3);
}

static void lookupLowersToBEIRArrayAccess() {
    auto program = baseProgram();
    auto& block = program.top.blocks[0];
    block.stmts.push_back(lookupStmt(4, var(3, intType(2)),
                                     {lit(3, intType(8)), lit(4, intType(8)), lit(5, intType(8))}));
    block.stmts.push_back(assign(2, var(4, intType(8))));

    auto beir_program = lowerToBEIR(program);
    CHECK(countKind(beir_program, beir::OperationKind::Lookup) == 0);
    CHECK(countKind(beir_program, beir::OperationKind::Aggregate) == 1);
    CHECK(countKind(beir_program, beir::OperationKind::ArrayAccess) == 1);
    CHECK(countKind(beir_program, beir::OperationKind::Ite) == 0);
    CHECK(countKind(beir_program, beir::OperationKind::Binary) == 0);
    bool found_array = false;
    for (const auto& signal : beir_program.signals) {
        if (signal.driver &&
            signal.driver->kind == beir::OperationKind::Aggregate) {
            CHECK(signal.type.width == 8);
            CHECK(signal.type.array_dims.size() == 1);
            CHECK(signal.type.array_dims[0] == 3);
            CHECK(signal.driver->operands.size() == 3);
            found_array = true;
        }
    }
    CHECK(found_array);
}

static void signedArithmeticShiftMapsToShrSignedView() {
    auto program = baseProgram();
    auto& block = program.top.blocks[0];
    block.stmts.push_back(opStmt(2, s8opnorm::S8OpKind::AShr, intType(8),
                                 {var(0, intType(8), true), lit(1, intType(8))}));

    auto beir_program = lowerToBEIR(program);
    bool found = false;
    for (const auto& signal : beir_program.signals) {
        if (!signal.driver ||
            signal.driver->kind != beir::OperationKind::Binary ||
            signal.driver->op != beir::OpCode::Shr) {
            continue;
        }
        CHECK(signal.driver->operands.size() == 2);
        CHECK(signal.driver->operands[0].signed_view);
        found = true;
    }
    CHECK(found);
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

static void outputInitialReadIsZeroTotalized() {
    s10predicate::S10PredicateProgram program;
    program.name = "bad";
    program.base_symbols.push_back(s10predicate::S10Symbol{0, intType(8), "out", s10predicate::S10SymbolRole::Port});
    program.values.push_back(predS10Value(0, 0, 0, intType(8), s10predicate::S10ValueKind::Initial, "out"));
    program.values.push_back(predS10Value(1, 0, 1, intType(8), s10predicate::S10ValueKind::Statement, "out"));

    s10predicate::S10Definition def;
    def.kind = s10predicate::S10DefKind::Assign;
    def.target = 1;
    def.guard = predLiteral(1, boolType());
    def.value = predValue(0, intType(8));
    program.definitions.push_back(std::move(def));
    program.ports.push_back(s10predicate::S10Port{0, ParamDirection::Output, ParamPassingKind::MutableRef, 0, 1, predLiteral(1, boolType())});

    auto result = s11beir::buildBEIR(program);
    CHECK(result.ok());
    CHECK(result.program.has_value());
    const auto* initial = findSignal(*result.program, "out_v0");
    CHECK(initial != nullptr);
    CHECK(initial->driver.has_value());
    CHECK(initial->driver->kind == beir::OperationKind::Assign);
    CHECK(initial->driver->operands.size() == 1);
    CHECK(initial->driver->operands[0].kind == beir::OperandKind::Literal);
    CHECK(!initial->driver->operands[0].constant.limbs.empty());
    CHECK(initial->driver->operands[0].constant.limbs[0] == 0);
}

static void groupedOutputArrayBuildsBEIRArrayPort() {
    s10predicate::S10PredicateProgram program;
    program.name = "manual_grouped_output";
    program.base_symbols.push_back(s10predicate::S10Symbol{0, intType(8), "out__idx_0", s10predicate::S10SymbolRole::Port});
    program.base_symbols.push_back(s10predicate::S10Symbol{1, intType(8), "out__idx_1", s10predicate::S10SymbolRole::Port});
    program.values.push_back(predS10Value(0, 0, 0, intType(8), s10predicate::S10ValueKind::Statement, "out__idx_0"));
    program.values.push_back(predS10Value(1, 1, 0, intType(8), s10predicate::S10ValueKind::Statement, "out__idx_1"));

    s10predicate::S10Definition def0;
    def0.kind = s10predicate::S10DefKind::Assign;
    def0.target = 0;
    def0.guard = predLiteral(1, boolType());
    def0.value = predLiteral(7, intType(8));
    program.definitions.push_back(std::move(def0));

    s10predicate::S10Definition def1;
    def1.kind = s10predicate::S10DefKind::Assign;
    def1.target = 1;
    def1.guard = predLiteral(1, boolType());
    def1.value = predLiteral(9, intType(8));
    program.definitions.push_back(std::move(def1));

    program.ports.push_back(s10predicate::S10Port{0, ParamDirection::Output, ParamPassingKind::MutableRef, std::nullopt, 0, predLiteral(1, boolType())});
    program.ports.push_back(s10predicate::S10Port{1, ParamDirection::Output, ParamPassingKind::MutableRef, std::nullopt, 1, predLiteral(1, boolType())});
    s10predicate::S10PortGroup group;
    group.source_name = "out";
    group.direction = ParamDirection::Output;
    group.passing = ParamPassingKind::MutableRef;
    group.scalar_type = intType(8);
    group.array_dims = {2};
    group.elements.push_back(s10predicate::S10PortElement{0, {0}});
    group.elements.push_back(s10predicate::S10PortElement{1, {1}});
    program.port_groups.push_back(std::move(group));

    auto result = s11beir::buildBEIR(program);
    if (!result.ok()) std::cerr << result.error->formatted << "\n";
    CHECK(result.ok());
    CHECK(result.program.has_value());
    const beir::Port* out = findPort(result.program.value(), "out");
    CHECK(out != nullptr);
    CHECK(out->direction == beir::PortDirection::Output);
    CHECK(out->type.width == 8);
    CHECK(out->type.array_dims.size() == 1);
    CHECK(out->type.array_dims[0] == 2);
    CHECK(out->element_nodes.size() == 2);
}

static void sourcePipelineRunsThroughBEIR() {
    auto beir_program = runSourceToBEIR("testv2/fixtures/s9ssa/source_ssa.logic.cpp");
    std::string text = beir::emitText(beir_program);
    CHECK(text.find("beir v1") != std::string::npos);
    CHECK(text.find("function hls_main") != std::string::npos);
    CHECK(text.find("Output out") != std::string::npos);
    CHECK(text.find("driver lookup") == std::string::npos);
    CHECK(countKind(beir_program, beir::OperationKind::Lookup) == 0);
    CHECK(countKind(beir_program, beir::OperationKind::Aggregate) >= 1);
    CHECK(countKind(beir_program, beir::OperationKind::ArrayAccess) >= 1);
}

static void sourcePipelinePreservesArrayPortGroupsInBEIR() {
    auto beir_program = runSourceToBEIR("testv2/fixtures/s11beir/source_array_ports.logic.cpp");
    const beir::Port* in = findPort(beir_program, "in");
    CHECK(in != nullptr);
    CHECK(in->direction == beir::PortDirection::Input);
    CHECK(in->type.width == 8);
    CHECK(in->type.array_dims.size() == 1);
    CHECK(in->type.array_dims[0] == 2);
    CHECK(in->element_nodes.size() == 2);

    const beir::Port* selected = findPort(beir_program, "selected");
    CHECK(selected != nullptr);
    CHECK(selected->direction == beir::PortDirection::Output);
    CHECK(selected->type.width == 8);
    CHECK(selected->type.array_dims.empty());
    CHECK(selected->element_nodes.size() == 1);
}

int main() {
    rtlgenConnectsScalarPortElementsWithoutArraySelect();
    straightLineBuildsPortsAndOutputAssign();
    lookupLowersToBEIRArrayAccess();
    signedArithmeticShiftMapsToShrSignedView();
    outputInitialReadIsZeroTotalized();
    groupedOutputArrayBuildsBEIRArrayPort();
    sourcePipelineRunsThroughBEIR();
    sourcePipelinePreservesArrayPortGroupsInBEIR();
    return 0;
}
