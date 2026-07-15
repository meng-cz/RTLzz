#include "ast/ASTBuilder.h"
#include "s1apinorm/S1APINorm.h"
#include "s2validate/S2Validate.h"
#include "s3statementize/S3Statementize.h"
#include "s4cfg/S4CFG.h"
#include "s5unroll/S5Unroll.h"
#include "s6inline/S6Inline.h"
#include "s7flatten/S7Flatten.h"

#include <cstdlib>
#include <iostream>
#include <memory>
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

static TypeInfo int8() {
    return make_hw_type("Int", 8, true);
}

static TypeInfo int32() {
    TypeInfo type;
    type.name = "int";
    type.width = 32;
    type.is_signed = true;
    type.hw_kind = "builtin";
    return type;
}

static TypeInfo voidType() {
    TypeInfo type;
    type.name = "void";
    return type;
}

static TypeInfo pairType() {
    TypeInfo type;
    type.name = "Pair";
    type.struct_name = "Pair";
    return type;
}

static TypeInfo int8Array3() {
    TypeInfo type = int8();
    type.is_array = true;
    type.array_dims = {3};
    type.array_size = 3;
    return type;
}

static TypeInfo arrayOf(TypeInfo elem, int size) {
    elem.is_array = true;
    elem.array_dims = {size};
    elem.array_size = size;
    return elem;
}

static TypeInfo pairArray2() {
    return arrayOf(pairType(), 2);
}

static TypeInfo packetType() {
    TypeInfo type;
    type.name = "Packet";
    type.struct_name = "Packet";
    return type;
}

static TypeInfo boxType() {
    TypeInfo type;
    type.name = "Box";
    type.struct_name = "Box";
    return type;
}

static TypeInfo rowType() {
    TypeInfo type;
    type.name = "Row";
    type.struct_name = "Row";
    return type;
}

static TypeInfo rowArray2() {
    return arrayOf(rowType(), 2);
}

static bool flatScalar(const TypeInfo& type) {
    return !type.is_array && !type.is_pointer && !type.is_reference &&
           type.struct_name.empty();
}

static s3statementize::SymbolInfo symbol(s3statementize::SymbolId id,
                                         const std::string& name,
                                         TypeInfo type,
                                         bool is_param = false) {
    s3statementize::SymbolInfo out;
    out.id = id;
    out.name = name;
    out.type = std::move(type);
    out.is_param = is_param;
    return out;
}

static s3statementize::Operand var(const s3statementize::SymbolInfo& symbol) {
    s3statementize::Operand out;
    out.kind = s3statementize::OperandKind::Var;
    out.var_name = symbol.name;
    out.var_symbol = symbol.id;
    out.type = symbol.type;
    return out;
}

static s3statementize::Operand literal(const std::string& value, TypeInfo type) {
    s3statementize::Operand out;
    out.kind = s3statementize::OperandKind::Literal;
    out.literal_value = value;
    out.type = std::move(type);
    return out;
}

static s3statementize::LValue lv(const s3statementize::SymbolInfo& symbol) {
    s3statementize::LValue out;
    out.root = symbol.name;
    out.root_symbol = symbol.id;
    out.type = symbol.type;
    return out;
}

static s3statementize::Operand read(s3statementize::LValue value) {
    s3statementize::Operand out;
    out.kind = s3statementize::OperandKind::LValueRead;
    out.type = value.type;
    out.lvalue = std::move(value);
    return out;
}

static s3statementize::S3StmtPtr decl(const s3statementize::SymbolInfo& symbol) {
    auto stmt = std::make_shared<s3statementize::S3Stmt>();
    stmt->kind = s3statementize::S3StmtKind::Decl;
    stmt->decl_name = symbol.name;
    stmt->decl_symbol = symbol.id;
    stmt->decl_type = symbol.type;
    return stmt;
}

static s3statementize::S3StmtPtr assign(s3statementize::LValue target,
                                        s3statementize::Operand value) {
    auto stmt = std::make_shared<s3statementize::S3Stmt>();
    stmt->kind = s3statementize::S3StmtKind::Assign;
    stmt->target = std::move(target);
    stmt->value = std::move(value);
    return stmt;
}

static s3statementize::S3StmtPtr addOp(const s3statementize::SymbolInfo& target,
                                       s3statementize::Operand lhs,
                                       s3statementize::Operand rhs) {
    auto stmt = std::make_shared<s3statementize::S3Stmt>();
    stmt->kind = s3statementize::S3StmtKind::Op;
    stmt->target = lv(target);
    stmt->op.kind = s3statementize::OpExpr::Kind::Binary;
    stmt->op.binary_op = s3statementize::BinaryOp::Add;
    stmt->op.type = target.type;
    stmt->op.operands.push_back(std::move(lhs));
    stmt->op.operands.push_back(std::move(rhs));
    return stmt;
}

static s6inline::InlinedCFGProgram baseProgram() {
    s6inline::InlinedCFGProgram program;
    program.top.name = "hls_main";
    program.top.return_type = voidType();
    program.top.entry = 0;
    program.top.exit = 0;
    program.struct_fields["Pair"] = {
        StructFieldInfo{"n", int8()},
        StructFieldInfo{"m", int8()},
    };
    program.top.symbols = {
        symbol(0, "a", pairType()),
        symbol(1, "dst", pairType()),
        symbol(2, "b", int8()),
        symbol(3, "c", int8()),
        symbol(4, "tmp", int8(), false),
        symbol(5, "arr", int8Array3(), true),
        symbol(6, "idx", int32(), true),
    };
    ParamDecl arr_param;
    arr_param.name = "arr";
    arr_param.type = int8Array3();
    arr_param.direction = ParamDirection::Input;
    arr_param.passing = ParamPassingKind::ConstRef;
    program.top.params.push_back(arr_param);
    ParamDecl idx_param;
    idx_param.name = "idx";
    idx_param.type = int32();
    idx_param.direction = ParamDirection::Input;
    idx_param.passing = ParamPassingKind::Value;
    program.top.params.push_back(idx_param);
    auto block = std::make_unique<s6inline::InlinedBasicBlock>();
    block->id = 0;
    block->terminator.kind = s4cfg::TermKind::Exit;
    program.top.blocks.push_back(std::move(block));
    return program;
}

static s3statementize::LValue field(const s3statementize::SymbolInfo& root,
                                    const std::string& name,
                                    TypeInfo type) {
    auto out = lv(root);
    out.type = std::move(type);
    s3statementize::LValueAccess access;
    access.kind = s3statementize::LValueAccessKind::Field;
    access.field = name;
    out.accesses.push_back(std::move(access));
    return out;
}

static s3statementize::LValue index(const s3statementize::SymbolInfo& root,
                                    s3statementize::Operand idx,
                                    TypeInfo type) {
    auto out = lv(root);
    out.type = std::move(type);
    s3statementize::LValueAccess access;
    access.kind = s3statementize::LValueAccessKind::Index;
    access.index = std::make_shared<s3statementize::Operand>(std::move(idx));
    out.accesses.push_back(std::move(access));
    return out;
}

static s3statementize::LValue appendField(s3statementize::LValue out,
                                          const std::string& name,
                                          TypeInfo type) {
    out.type = std::move(type);
    s3statementize::LValueAccess access;
    access.kind = s3statementize::LValueAccessKind::Field;
    access.field = name;
    out.accesses.push_back(std::move(access));
    return out;
}

static s3statementize::LValue appendIndex(s3statementize::LValue out,
                                          s3statementize::Operand idx,
                                          TypeInfo type) {
    out.type = std::move(type);
    s3statementize::LValueAccess access;
    access.kind = s3statementize::LValueAccessKind::Index;
    access.index = std::make_shared<s3statementize::Operand>(std::move(idx));
    out.accesses.push_back(std::move(access));
    return out;
}

static std::string flattenDebug(s6inline::InlinedCFGProgram program) {
    s7flatten::FlattenOptions options;
    options.debug_print = true;
    auto result = s7flatten::flattenProgram(program, options);
    if (!result.ok()) std::cerr << result.error->formatted << "\n";
    CHECK(result.ok());
    CHECK(result.program.has_value());
    CHECK(!result.debug_text.empty());
    const auto& fn = result.program->top;
    for (std::size_t i = 0; i < fn.symbols.size(); ++i) {
        CHECK(fn.symbols[i].id == static_cast<s7flatten::SymbolId>(i));
        CHECK(flatScalar(fn.symbols[i].type));
    }
    for (const auto& port : fn.ports) {
        CHECK(port.symbol >= 0);
        CHECK(port.symbol < static_cast<s7flatten::SymbolId>(fn.symbols.size()));
    }
    return result.debug_text;
}

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

static std::string runASTToS7(FunctionAST ast);

static std::string runSourceToS7(const std::string& file) {
    auto ast = parseFixture(file);
    return runASTToS7(std::move(ast));
}

static std::string runASTToS7(FunctionAST ast) {
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
    s7flatten::FlattenOptions options;
    options.debug_print = true;
    auto s7 = s7flatten::flattenProgram(s6.program.value(), options);
    if (!s7.ok()) std::cerr << s7.error->formatted << "\n";
    CHECK(s7.ok());
    CHECK(s7.program.has_value());
    CHECK(!s7.debug_text.empty());
    return s7.debug_text;
}

static ParamDecl valueParam(const std::string& name, TypeInfo type) {
    ParamDecl out;
    out.name = name;
    out.type = std::move(type);
    out.direction = ParamDirection::Input;
    out.passing = ParamPassingKind::Value;
    return out;
}

static ParamDecl outputRefParam(const std::string& name, TypeInfo type) {
    ParamDecl out = valueParam(name, std::move(type));
    out.type.is_reference = true;
    out.direction = ParamDirection::Output;
    out.passing = ParamPassingKind::MutableRef;
    out.is_reference = true;
    out.is_output = true;
    return out;
}

static ExprPtr astCall(const std::string& callee,
                       std::vector<ExprPtr> args,
                       TypeInfo type) {
    auto expr = std::make_shared<Expr>();
    expr->kind = ExprKind::Call;
    expr->callee = callee;
    expr->args = std::move(args);
    expr->type = std::move(type);
    return expr;
}

static StmtPtr astDecl(const std::string& name, TypeInfo type) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Decl;
    stmt->decl_name = name;
    stmt->decl_type = std::move(type);
    return stmt;
}

static StmtPtr astAssign(ExprPtr target, ExprPtr value) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Assign;
    stmt->assign_target = std::move(target);
    stmt->assign_value = std::move(value);
    return stmt;
}

static StmtPtr astReturn(ExprPtr value) {
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Return;
    stmt->return_value = std::move(value);
    return stmt;
}

static ExprPtr astField(ExprPtr base, const std::string& field_name, TypeInfo type) {
    return make_field_access(std::move(base), field_name, std::move(type));
}

static ExprPtr astIndex(ExprPtr base, ExprPtr index, TypeInfo type) {
    return make_array_access(std::move(base), std::move(index), std::move(type));
}

static ExprPtr astPacketLane(const std::string& root,
                             ExprPtr index_expr,
                             TypeInfo root_type = packetType()) {
    return astIndex(astField(make_var(root, std::move(root_type)), "lanes", pairArray2()),
                    std::move(index_expr), pairType());
}

static FunctionAST makeAggregateLambdaProgram() {
    FunctionAST top;
    top.name = "hls_main";
    top.return_type = voidType();
    top.params.push_back(valueParam("seed", int8()));
    top.params.push_back(valueParam("idx", int32()));
    top.params.push_back(outputRefParam("out", int8()));
    top.struct_fields["Pair"] = {
        StructFieldInfo{"n", int8()},
        StructFieldInfo{"m", int8()},
    };
    top.struct_fields["Packet"] = {
        StructFieldInfo{"lanes", pairArray2()},
        StructFieldInfo{"tail", int8()},
    };

    auto make_packet = std::make_shared<FunctionAST>();
    make_packet->name = "make_packet";
    make_packet->return_type = packetType();
    make_packet->params.push_back(valueParam("seed", int8()));
    make_packet->body.push_back(astDecl("pkt", packetType()));
    make_packet->body.push_back(astAssign(
        astField(astPacketLane("pkt", make_literal("0", int32())), "n", int8()),
        make_var("seed", int8())));
    make_packet->body.push_back(astAssign(
        astField(astPacketLane("pkt", make_literal("0", int32())), "m", int8()),
        make_binary("+", make_var("seed", int8()), make_literal("1", int8()), int8())));
    make_packet->body.push_back(astAssign(
        astField(astPacketLane("pkt", make_literal("1", int32())), "n", int8()),
        make_binary("+", make_var("seed", int8()), make_literal("2", int8()), int8())));
    make_packet->body.push_back(astAssign(
        astField(astPacketLane("pkt", make_literal("1", int32())), "m", int8()),
        make_binary("+", make_var("seed", int8()), make_literal("3", int8()), int8())));
    make_packet->body.push_back(astAssign(
        astField(make_var("pkt", packetType()), "tail", int8()),
        make_binary("+", make_var("seed", int8()), make_literal("4", int8()), int8())));
    make_packet->body.push_back(astReturn(make_var("pkt", packetType())));
    top.helpers.push_back(make_packet);

    auto lambda = std::make_shared<FunctionAST>();
    lambda->name = "select_lambda";
    lambda->return_type = pairType();
    lambda->params.push_back(valueParam("pkt_arg", packetType()));
    lambda->params.push_back(valueParam("idx_arg", int32()));
    lambda->body.push_back(astDecl("local", pairType()));
    lambda->body.push_back(astAssign(
        make_var("local", pairType()),
        astPacketLane("pkt_arg", make_var("idx_arg", int32()))));
    lambda->body.push_back(astReturn(make_var("local", pairType())));
    top.lambdas["select_lambda"] = lambda;

    top.body.push_back(astDecl("pkt", packetType()));
    top.body.push_back(astAssign(
        make_var("pkt", packetType()),
        astCall("make_packet", {make_var("seed", int8())}, packetType())));
    top.body.push_back(astDecl("chosen", pairType()));
    top.body.push_back(astAssign(
        make_var("chosen", pairType()),
        astCall("select_lambda", {make_var("pkt", packetType()), make_var("idx", int32())},
                pairType())));
    top.body.push_back(astAssign(
        make_var("out", int8()),
        make_binary("+",
                    astField(astPacketLane("pkt", make_var("idx", int32())), "n", int8()),
                    astField(make_var("chosen", pairType()), "m", int8()),
                    int8())));
    return top;
}

static void fieldReadAndAggregateCopyFlatten() {
    auto program = baseProgram();
    const auto& syms = program.top.symbols;
    auto& stmts = program.top.blocks[0]->stmts;
    for (const auto& sym : syms) stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Decl, decl(sym)});
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Op,
        addOp(syms[4], read(field(syms[0], "n", int8())), var(syms[2]))});
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(lv(syms[3]), var(syms[4]))});
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(lv(syms[1]), var(syms[0]))});

    auto debug = flattenDebug(std::move(program));
    CHECK(debug.find(" a__n\n") != std::string::npos);
    CHECK(debug.find(" a__m\n") != std::string::npos);
    CHECK(debug.find(" dst__n\n") != std::string::npos);
    CHECK(debug.find(" dst__m\n") != std::string::npos);
    CHECK(debug.find("op tmp = Add(a__n, b)") != std::string::npos);
    CHECK(debug.find("assign dst__n = a__n") != std::string::npos);
    CHECK(debug.find("assign dst__m = a__m") != std::string::npos);
    CHECK(debug.find("a.n") == std::string::npos);
}

static void staticAndDynamicArrayAccessFlatten() {
    auto program = baseProgram();
    const auto& syms = program.top.symbols;
    auto& stmts = program.top.blocks[0]->stmts;
    for (const auto& sym : syms) stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Decl, decl(sym)});
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(lv(syms[3]), read(index(syms[5], literal("1", int32()), int8())))});
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(lv(syms[3]), read(index(syms[5], var(syms[6]), int8())))});
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(index(syms[5], var(syms[6]), int8()), var(syms[3]))});

    auto debug = flattenDebug(std::move(program));
    CHECK(debug.find(" arr__idx_0\n") != std::string::npos);
    CHECK(debug.find(" arr__idx_1\n") != std::string::npos);
    CHECK(debug.find(" arr__idx_2\n") != std::string::npos);
    CHECK(debug.find("assign c = arr__idx_1") != std::string::npos);
    CHECK(debug.find("lookup c = lookup(idx, arr__idx_0, arr__idx_1, arr__idx_2)") != std::string::npos);
    CHECK(debug.find("lookupwrite [arr__idx_0, arr__idx_1, arr__idx_2] = lookupwrite(idx, c, arr__idx_0, arr__idx_1, arr__idx_2)") != std::string::npos);
}

static void nestedStructArrayAccessFlatten() {
    s6inline::InlinedCFGProgram program;
    program.top.name = "hls_main";
    program.top.return_type = voidType();
    program.top.entry = 0;
    program.top.exit = 0;
    program.struct_fields["Pair"] = {
        StructFieldInfo{"n", int8()},
        StructFieldInfo{"m", int8()},
    };
    program.struct_fields["Packet"] = {
        StructFieldInfo{"lanes", pairArray2()},
        StructFieldInfo{"tail", int8()},
    };
    program.struct_fields["Box"] = {
        StructFieldInfo{"pkts", arrayOf(packetType(), 2)},
    };
    program.top.symbols = {
        symbol(0, "pkt", packetType()),
        symbol(1, "dst", pairType()),
        symbol(2, "box", boxType()),
        symbol(3, "idx", int32(), true),
        symbol(4, "out", int8(), true),
    };
    ParamDecl idx_param;
    idx_param.name = "idx";
    idx_param.type = int32();
    idx_param.direction = ParamDirection::Input;
    idx_param.passing = ParamPassingKind::Value;
    program.top.params.push_back(idx_param);
    ParamDecl out_param;
    out_param.name = "out";
    out_param.type = int8();
    out_param.direction = ParamDirection::Output;
    out_param.passing = ParamPassingKind::MutableRef;
    program.top.params.push_back(out_param);

    auto block = std::make_unique<s6inline::InlinedBasicBlock>();
    block->id = 0;
    block->terminator.kind = s4cfg::TermKind::Exit;
    auto& stmts = block->stmts;
    for (const auto& sym : program.top.symbols) {
        stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Decl, decl(sym)});
    }

    auto pkt_lanes = appendField(lv(program.top.symbols[0]), "lanes", pairArray2());
    auto pkt_lanes_1 = appendIndex(pkt_lanes, literal("1", int32()), pairType());
    auto pkt_lanes_1_m = appendField(pkt_lanes_1, "m", int8());
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(lv(program.top.symbols[4]), read(pkt_lanes_1_m))});

    auto pkt_lanes_dyn = appendIndex(pkt_lanes, var(program.top.symbols[3]), pairType());
    auto pkt_lanes_dyn_n = appendField(pkt_lanes_dyn, "n", int8());
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(lv(program.top.symbols[4]), read(pkt_lanes_dyn_n))});

    auto pkt_lanes_dyn_m = appendField(pkt_lanes_dyn, "m", int8());
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(pkt_lanes_dyn_m, var(program.top.symbols[4]))});

    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(lv(program.top.symbols[1]), read(pkt_lanes_dyn))});
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(pkt_lanes_dyn, var(program.top.symbols[1]))});

    auto box_pkts = appendField(lv(program.top.symbols[2]), "pkts", arrayOf(packetType(), 2));
    auto box_pkts_dyn = appendIndex(box_pkts, var(program.top.symbols[3]), packetType());
    auto box_pkts_dyn_lanes = appendField(box_pkts_dyn, "lanes", pairArray2());
    auto box_pkts_dyn_lanes_1 = appendIndex(box_pkts_dyn_lanes, literal("1", int32()), pairType());
    auto box_pkts_dyn_lanes_1_m = appendField(box_pkts_dyn_lanes_1, "m", int8());
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(lv(program.top.symbols[4]), read(box_pkts_dyn_lanes_1_m))});

    program.top.blocks.push_back(std::move(block));

    auto debug = flattenDebug(std::move(program));
    CHECK(debug.find(" pkt__lanes__idx_0__n\n") != std::string::npos);
    CHECK(debug.find(" pkt__lanes__idx_0__m\n") != std::string::npos);
    CHECK(debug.find(" pkt__lanes__idx_1__n\n") != std::string::npos);
    CHECK(debug.find(" pkt__lanes__idx_1__m\n") != std::string::npos);
    CHECK(debug.find(" pkt__tail\n") != std::string::npos);
    CHECK(debug.find(" box__pkts__idx_0__lanes__idx_0__n\n") != std::string::npos);
    CHECK(debug.find(" box__pkts__idx_0__lanes__idx_0__m\n") != std::string::npos);
    CHECK(debug.find("assign out = pkt__lanes__idx_1__m") != std::string::npos);
    CHECK(debug.find("lookup out = lookup(idx, pkt__lanes__idx_0__n, pkt__lanes__idx_1__n)") != std::string::npos);
    CHECK(debug.find("lookupwrite [pkt__lanes__idx_0__m, pkt__lanes__idx_1__m] = lookupwrite(idx, out, pkt__lanes__idx_0__m, pkt__lanes__idx_1__m)") != std::string::npos);
    CHECK(debug.find("lookup dst__n = lookup(idx, pkt__lanes__idx_0__n, pkt__lanes__idx_1__n)") != std::string::npos);
    CHECK(debug.find("lookup dst__m = lookup(idx, pkt__lanes__idx_0__m, pkt__lanes__idx_1__m)") != std::string::npos);
    CHECK(debug.find("lookupwrite [pkt__lanes__idx_0__n, pkt__lanes__idx_1__n] = lookupwrite(idx, dst__n, pkt__lanes__idx_0__n, pkt__lanes__idx_1__n)") != std::string::npos);
    CHECK(debug.find("lookupwrite [pkt__lanes__idx_0__m, pkt__lanes__idx_1__m] = lookupwrite(idx, dst__m, pkt__lanes__idx_0__m, pkt__lanes__idx_1__m)") != std::string::npos);
    CHECK(debug.find("lookup out = lookup(idx, box__pkts__idx_0__lanes__idx_1__m, box__pkts__idx_1__lanes__idx_1__m)") != std::string::npos);
}

static void multiDynamicIndexReadAndWriteFlatten() {
    s6inline::InlinedCFGProgram program;
    program.top.name = "hls_main";
    program.top.return_type = voidType();
    program.top.entry = 0;
    program.top.exit = 0;
    program.struct_fields["Row"] = {
        StructFieldInfo{"c", arrayOf(int8(), 2)},
    };
    program.top.symbols = {
        symbol(0, "rows", rowArray2()),
        symbol(1, "i", int32(), true),
        symbol(2, "j", int32(), true),
        symbol(3, "out", int8(), true),
    };
    ParamDecl i_param;
    i_param.name = "i";
    i_param.type = int32();
    i_param.direction = ParamDirection::Input;
    i_param.passing = ParamPassingKind::Value;
    program.top.params.push_back(i_param);
    ParamDecl j_param;
    j_param.name = "j";
    j_param.type = int32();
    j_param.direction = ParamDirection::Input;
    j_param.passing = ParamPassingKind::Value;
    program.top.params.push_back(j_param);
    ParamDecl out_param;
    out_param.name = "out";
    out_param.type = int8();
    out_param.direction = ParamDirection::Output;
    out_param.passing = ParamPassingKind::MutableRef;
    program.top.params.push_back(out_param);

    auto block = std::make_unique<s6inline::InlinedBasicBlock>();
    block->id = 0;
    block->terminator.kind = s4cfg::TermKind::Exit;
    auto& stmts = block->stmts;
    for (const auto& sym : program.top.symbols) {
        stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Decl, decl(sym)});
    }

    auto rows_i = appendIndex(lv(program.top.symbols[0]), var(program.top.symbols[1]), rowType());
    auto rows_i_c = appendField(rows_i, "c", arrayOf(int8(), 2));
    auto rows_i_c_j = appendIndex(rows_i_c, var(program.top.symbols[2]), int8());
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(lv(program.top.symbols[3]), read(rows_i_c_j))});
    stmts.push_back(s4cfg::CFGStmt{s4cfg::CFGStmtKind::Assign,
        assign(rows_i_c_j, var(program.top.symbols[3]))});
    program.top.blocks.push_back(std::move(block));

    auto debug = flattenDebug(std::move(program));
    CHECK(debug.find(" rows__idx_0__c__idx_0\n") != std::string::npos);
    CHECK(debug.find(" rows__idx_0__c__idx_1\n") != std::string::npos);
    CHECK(debug.find(" rows__idx_1__c__idx_0\n") != std::string::npos);
    CHECK(debug.find(" rows__idx_1__c__idx_1\n") != std::string::npos);
    CHECK(debug.find("lookup __s7_flatten_lookup_") != std::string::npos);
    CHECK(debug.find("lookup out = lookup(i, __s7_flatten_lookup_") != std::string::npos);
    CHECK(debug.find("lookupwrite [__s7_flatten_lookupwrite_") != std::string::npos);
    CHECK(debug.find("lookupwrite [rows__idx_0__c__idx_0, rows__idx_1__c__idx_0]") != std::string::npos);
    CHECK(debug.find("lookupwrite [rows__idx_0__c__idx_1, rows__idx_1__c__idx_1]") != std::string::npos);
}

static void sourcePipelineFlattensStructAndArray() {
    auto debug = runSourceToS7("testv2/fixtures/s7flatten/source_flatten.logic.cpp");
    CHECK(debug.find(" a__n\n") != std::string::npos);
    CHECK(debug.find(" a__m\n") != std::string::npos);
    CHECK(debug.find(" b__n\n") != std::string::npos);
    CHECK(debug.find(" b__m\n") != std::string::npos);
    CHECK(debug.find(" arr__idx_0\n") != std::string::npos);
    CHECK(debug.find(" arr__idx_1\n") != std::string::npos);
    CHECK(debug.find(" arr__idx_2\n") != std::string::npos);
    CHECK(debug.find("lookup ") != std::string::npos);
    CHECK(debug.find("lookupwrite [arr__idx_0, arr__idx_1, arr__idx_2]") != std::string::npos);
    CHECK(debug.find(".n") == std::string::npos);
    CHECK(debug.find("[") == std::string::npos ||
          debug.find("lookupwrite [arr__idx_0, arr__idx_1, arr__idx_2]") != std::string::npos);
}

static void sourcePipelineFlattensComplexAggregateCalls() {
    auto debug = runSourceToS7("testv2/fixtures/s7flatten/source_complex_aggregates.logic.cpp");
    CHECK(debug.find(" pkt__lanes__idx_0__n\n") != std::string::npos);
    CHECK(debug.find(" pkt__lanes__idx_0__m\n") != std::string::npos);
    CHECK(debug.find(" pkt__lanes__idx_1__n\n") != std::string::npos);
    CHECK(debug.find(" pkt__lanes__idx_1__m\n") != std::string::npos);
    CHECK(debug.find(" pkt__tail\n") != std::string::npos);
    CHECK(debug.find(" chosen__n\n") != std::string::npos);
    CHECK(debug.find(" chosen__m\n") != std::string::npos);
    CHECK(debug.find(" box__pkts__idx_0__lanes__idx_0__n\n") != std::string::npos);
    CHECK(debug.find(" box__pkts__idx_0__lanes__idx_0__m\n") != std::string::npos);
    CHECK(debug.find("__s6_make_packet_") != std::string::npos);
    CHECK(debug.find("__s6_select_") != std::string::npos);
    CHECK(debug.find("__s6_touch_packet_") != std::string::npos);
    CHECK(debug.find("lookup ") != std::string::npos);
    CHECK(debug.find("lookupwrite ") != std::string::npos);
    CHECK(debug.find("__lanes__idx_0__n") != std::string::npos);
    CHECK(debug.find("__pkts__idx_0__lanes__idx_1__m") != std::string::npos);
    CHECK(debug.find(".lanes") == std::string::npos);
    CHECK(debug.find(".pkts") == std::string::npos);
}

static void astPipelineFlattensAggregateLambdaCalls() {
    auto debug = runASTToS7(makeAggregateLambdaProgram());
    CHECK(debug.find(" pkt__lanes__idx_0__n\n") != std::string::npos);
    CHECK(debug.find(" pkt__lanes__idx_0__m\n") != std::string::npos);
    CHECK(debug.find(" pkt__lanes__idx_1__n\n") != std::string::npos);
    CHECK(debug.find(" pkt__lanes__idx_1__m\n") != std::string::npos);
    CHECK(debug.find(" pkt__tail\n") != std::string::npos);
    CHECK(debug.find(" chosen__n\n") != std::string::npos);
    CHECK(debug.find(" chosen__m\n") != std::string::npos);
    CHECK(debug.find("__s6_make_packet_") != std::string::npos);
    CHECK(debug.find("__s6_select_lambda_") != std::string::npos);
    CHECK(debug.find("lookup __s6_select_lambda_") != std::string::npos);
    CHECK(debug.find("local___n = lookup") != std::string::npos);
    CHECK(debug.find("lookup ") != std::string::npos);
    CHECK(debug.find(".lanes") == std::string::npos);
}

int main() {
    fieldReadAndAggregateCopyFlatten();
    staticAndDynamicArrayAccessFlatten();
    nestedStructArrayAccessFlatten();
    multiDynamicIndexReadAndWriteFlatten();
    sourcePipelineFlattensStructAndArray();
    sourcePipelineFlattensComplexAggregateCalls();
    astPipelineFlattensAggregateLambdaCalls();
    return 0;
}
