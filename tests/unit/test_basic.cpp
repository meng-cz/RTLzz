#include "ast/AST.h"
#include "ir/CFG.h"
#include "ir/SSA.h"
#include "transform/LoopUnroll.h"
#include "transform/Normalize.h"
#include "transform/Predicate.h"
#include "predicate/OutputExpressionMap.h"
#include "predicate/PredicateVerifier.h"
#include "emitter/ListJsonEmitter.h"
#include "eval/PredicateEvaluator.h"
#include "ir/CanonicalIR.h"
#include "ir/TypedDAG.h"
#include "normalize/NormalizeUtils.h"
#include "semantics/AliasGraph.h"
#include "semantics/IntSemantics.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
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

static TypeInfo u(int w) {
    return TypeInfo{"UInt<" + std::to_string(w) + ">", w, false};
}

static TypeInfo i(int w) {
    return make_hw_type("Int", w, false);
}

static TypeInfo signedView(int w) {
    auto t = make_hw_type("Int", w, true);
    t.name = "IntSignedView<" + std::to_string(w) + ">";
    t.hw_kind = "signed_view";
    return t;
}

static TypeInfo boolean() {
    return TypeInfo{"bool", 1, false};
}

static StmtPtr assign(const std::string& dst, ExprPtr value, TypeInfo type) {
    auto s = std::make_shared<Stmt>();
    s->kind = StmtKind::Assign;
    s->assign_target = make_var(dst, type);
    s->assign_value = std::move(value);
    return s;
}

static StmtPtr decl(const std::string& name, TypeInfo type, ExprPtr init = nullptr) {
    auto s = std::make_shared<Stmt>();
    s->kind = StmtKind::Decl;
    s->decl_name = name;
    s->decl_type = type;
    if (init) s->decl_init = init;
    return s;
}

static ParamDecl param(TypeInfo type,
                       const std::string& name,
                       ParamDirection direction = ParamDirection::Input) {
    ParamDecl p;
    p.type = std::move(type);
    p.name = name;
    p.direction = direction;
    p.is_output = direction != ParamDirection::Input;
    return p;
}

static FunctionAST fn(std::vector<StmtPtr> body) {
    FunctionAST f;
    f.name = "comb";
    f.return_type = TypeInfo{"void", 0, false};
    f.params.push_back(param(u(8), "a"));
    f.params.push_back(param(u(8), "b"));
    f.params.push_back(param(boolean(), "sel"));
    f.params.push_back(param(u(8), "out", ParamDirection::Output));
    f.body = std::move(body);
    return f;
}

struct PipeResult {
    PredicateProgram prog;
    std::string text;
    std::string json;
    std::string smt;
    std::string error;
};

static std::string envValue(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, name) != 0 || !value) return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
#endif
}

static PipeResult runPipeline(FunctionAST f, int unroll_limit = 64) {
    PipeResult r;
    auto trace = [&](const char* stage) {
        if (!envValue("GPEF_UNIT_TRACE").empty()) std::cerr << "    stage: " << stage << std::endl;
    };
    auto traceCount = [&](const char* name, size_t value) {
        if (!envValue("GPEF_UNIT_TRACE").empty()) std::cerr << "    " << name << ": " << value << std::endl;
    };
    UnrollConfig cfg;
    cfg.max_iterations = unroll_limit;
    trace("unroll.begin");
    auto unrolled = unrollLoops(f.body, cfg);
    trace("unroll.end");
    traceCount("unrolled.statements", unrolled.body.size());
    if (!unrolled.error.empty()) {
        r.error = unrolled.error;
        return r;
    }
    trace("normalize.begin");
    auto normalized = normalizeFunction(f, unrolled.body);
    trace("normalize.end");
    traceCount("normalized.statements", normalized.body.size());
    if (!normalized.error.empty()) {
        r.error = normalized.error;
        return r;
    }
    trace("cfg.begin");
    auto cfg_graph = buildCFG(normalized.body);
    trace("cfg.end");
    traceCount("cfg.blocks", cfg_graph.blocks.size());
    trace("ssa.begin");
    auto ssa = buildSSA(cfg_graph, normalized.ssa_seed_symbols);
    if (!ssa.error.empty()) {
        r.error = ssa.error;
        return r;
    }
    trace("ssa.end");
    traceCount("ssa.blocks", ssa.blocks.size());
    trace("predicate.begin");
    r.prog = predicate(ssa);
    trace("predicate.end");
    traceCount("predicate.assignments", r.prog.assignments.size());
    r.prog.function_name = f.name;
    for (auto& [name, type] : normalized.symbols) r.prog.symbols[name] = type;
    r.prog.param_directions = normalized.param_directions;
    r.prog.output_default_reasons = normalized.output_default_reasons;
    r.prog.output_paired_controls = normalized.output_paired_controls;
    r.prog.outputs = normalized.output_params;
    trace("output_map.begin");
    buildOutputExpressionMap(r.prog);
    trace("output_map.end");
    trace("verify.begin");
    auto verified = verifyPredicateProgram(r.prog);
    trace("verify.end");
    if (!verified.ok) {
        r.error = verified.error;
        return r;
    }
    trace("listjson.begin");
    r.json = emitListJson(r.prog);
    r.text = r.json;
    r.smt = r.json;
    trace("listjson.end");
    return r;
}

static void checkContains(const std::string& s, const std::string& needle) {
    if (s.find(needle) == std::string::npos) {
        std::cerr << "Missing expected text: " << needle << "\n";
    }
    CHECK(s.find(needle) != std::string::npos);
}

static std::string shellQuote(const std::filesystem::path& path) {
    std::string s = path.string();
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

static int runShellCommand(const std::string& command) {
#ifdef _WIN32
    return std::system(("cmd /C \"" + command + "\"").c_str());
#else
    return std::system(command.c_str());
#endif
}

static std::string findUintOracleCompiler() {
    std::vector<std::string> candidates;
    if (!envValue("GPEF_UINT_ORACLE_CXX").empty()) candidates.push_back(envValue("GPEF_UINT_ORACLE_CXX"));
    if (!envValue("CXX").empty()) candidates.push_back(envValue("CXX"));
#ifdef _WIN32
    if (std::filesystem::exists("C:\\Program Files\\LLVM\\bin\\clang++.exe")) {
        return "C:\\Program Files\\LLVM\\bin\\clang++.exe";
    }
    candidates.push_back("C:\\Program Files\\LLVM\\bin\\clang++.exe");
#endif
    candidates.push_back("clang++");

    auto temp = std::filesystem::temp_directory_path() / "gpef_clangxx_probe.txt";
    for (const auto& c : candidates) {
        std::filesystem::path p(c);
        std::string exe = (c.find('\\') != std::string::npos || c.find('/') != std::string::npos)
            ? shellQuote(p)
            : c;
#ifdef _WIN32
        std::string cmd = exe + " --version > " + shellQuote(temp) + " 2>NUL";
#else
        std::string cmd = exe + " --version > " + shellQuote(temp) + " 2>/dev/null";
#endif
        if (runShellCommand(cmd) == 0) {
            std::filesystem::remove(temp);
            return c;
        }
    }
    std::filesystem::remove(temp);
    return {};
}

static std::uint64_t evalBitsToU64(const EvalBits& value) {
    return value.limbs.empty() ? 0 : value.limbs.front();
}

static std::uint64_t evalToU64(PredicateEvaluator& evaluator, const ExprPtr& expr) {
    return evalBitsToU64(evaluator.eval(expr));
}

struct EvalValue {
    std::uint64_t bits = 0;
    int width = 0;
    bool is_signed = false;
};

using EvalVars = std::unordered_map<std::string, EvalValue>;

static std::uint64_t evalMask(int width) {
    if (width <= 0) return 0;
    if (width >= 64) return ~std::uint64_t{0};
    return (std::uint64_t{1} << width) - 1;
}

static std::int64_t evalSigned(EvalValue v) {
    if (v.width <= 0) return 0;
    v.bits &= evalMask(v.width);
    if (v.width >= 64) return static_cast<std::int64_t>(v.bits);
    std::uint64_t sign = std::uint64_t{1} << (v.width - 1);
    if ((v.bits & sign) == 0) return static_cast<std::int64_t>(v.bits);
    return static_cast<std::int64_t>(v.bits | (~evalMask(v.width)));
}

static EvalValue evalMake(std::uint64_t bits, TypeInfo type) {
    EvalValue v{bits, type.width, type.is_signed || type.hw_kind == "signed_view"};
    if (v.width <= 0) v.width = 64;
    v.bits &= evalMask(v.width);
    return v;
}

static std::uint64_t evalParseLiteral(const std::string& text) {
    if (text == "true") return 1;
    if (text == "false") return 0;
    return static_cast<std::uint64_t>(std::stoull(text, nullptr, 0));
}

static EvalValue evalExpr(const ExprPtr& e,
                          const EvalVars& vars,
                          const std::unordered_map<std::string, std::vector<std::string>>& lookup_tables);

static EvalValue evalUnaryExpr(const ExprPtr& e,
                               const EvalVars& vars,
                               const std::unordered_map<std::string, std::vector<std::string>>& lookup_tables) {
    auto v = evalExpr(e->operand, vars, lookup_tables);
    if (e->op == "!") return evalMake(v.bits == 0 ? 1 : 0, boolean());
    if (e->op == "~") return evalMake(~v.bits, e->type.width > 0 ? e->type : e->operand->type);
    if (e->op == "-") return evalMake(0 - v.bits, e->type.width > 0 ? e->type : e->operand->type);
    throw std::runtime_error("unsupported unary op in Predicate IR evaluator: " + e->op);
}

static EvalValue evalBinaryExpr(const ExprPtr& e,
                                const EvalVars& vars,
                                const std::unordered_map<std::string, std::vector<std::string>>& lookup_tables) {
    auto l = evalExpr(e->left, vars, lookup_tables);
    auto r = evalExpr(e->right, vars, lookup_tables);
    TypeInfo type = e->type.width > 0 ? e->type : TypeInfo{"UInt<64>", 64, false};
    const bool signed_semantics = l.is_signed || r.is_signed || e->type.hw_kind == "signed_view";

    if (e->op == "+") return evalMake(l.bits + r.bits, type);
    if (e->op == "-") return evalMake(l.bits - r.bits, type);
    if (e->op == "*") return evalMake(l.bits * r.bits, type);
    if (e->op == "&") return evalMake(l.bits & r.bits, type);
    if (e->op == "|") return evalMake(l.bits | r.bits, type);
    if (e->op == "^") return evalMake(l.bits ^ r.bits, type);
    if (e->op == "&&") return evalMake((l.bits != 0 && r.bits != 0) ? 1 : 0, boolean());
    if (e->op == "||") return evalMake((l.bits != 0 || r.bits != 0) ? 1 : 0, boolean());
    if (e->op == "<<") {
        int shift = static_cast<int>(r.bits);
        return evalMake(shift >= l.width ? 0 : (l.bits << shift), type);
    }
    if (e->op == ">>") {
        int shift = static_cast<int>(r.bits);
        if (signed_semantics) {
            auto sv = evalSigned(l);
            if (shift >= l.width) return evalMake(sv < 0 ? evalMask(type.width) : 0, type);
            return evalMake(static_cast<std::uint64_t>(sv >> shift), type);
        }
        return evalMake(shift >= l.width ? 0 : (l.bits >> shift), type);
    }

    if (e->op == "==") return evalMake(l.bits == r.bits ? 1 : 0, boolean());
    if (e->op == "!=") return evalMake(l.bits != r.bits ? 1 : 0, boolean());
    if (e->op == "<") {
        return evalMake(signed_semantics ? (evalSigned(l) < evalSigned(r)) : (l.bits < r.bits), boolean());
    }
    if (e->op == "<=") {
        return evalMake(signed_semantics ? (evalSigned(l) <= evalSigned(r)) : (l.bits <= r.bits), boolean());
    }
    if (e->op == ">") {
        return evalMake(signed_semantics ? (evalSigned(l) > evalSigned(r)) : (l.bits > r.bits), boolean());
    }
    if (e->op == ">=") {
        return evalMake(signed_semantics ? (evalSigned(l) >= evalSigned(r)) : (l.bits >= r.bits), boolean());
    }
    throw std::runtime_error("unsupported binary op in Predicate IR evaluator: " + e->op);
}

static EvalValue evalExpr(const ExprPtr& e,
                          const EvalVars& vars,
                          const std::unordered_map<std::string, std::vector<std::string>>& lookup_tables) {
    if (!e) throw std::runtime_error("null expression in Predicate IR evaluator");
    switch (e->kind) {
    case ExprKind::Literal:
        return evalMake(evalParseLiteral(e->literal_value), e->type.width > 0 ? e->type : TypeInfo{"UInt<64>", 64, false});
    case ExprKind::VarRef: {
        auto it = vars.find(e->var_name);
        if (it == vars.end()) throw std::runtime_error("unknown variable in Predicate IR evaluator: " + e->var_name);
        auto v = it->second;
        if (e->type.width > 0) {
            v.width = e->type.width;
            v.is_signed = e->type.is_signed || e->type.hw_kind == "signed_view";
            v.bits &= evalMask(v.width);
        }
        return v;
    }
    case ExprKind::UnaryOp:
        return evalUnaryExpr(e, vars, lookup_tables);
    case ExprKind::BinaryOp:
        return evalBinaryExpr(e, vars, lookup_tables);
    case ExprKind::Ternary: {
        auto c = evalExpr(e->cond, vars, lookup_tables);
        return evalExpr(c.bits != 0 ? e->then_expr : e->else_expr, vars, lookup_tables);
    }
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc: {
        auto v = evalExpr(e->cast_expr, vars, lookup_tables);
        TypeInfo type = e->type;
        type.width = e->to_width;
        type.is_signed = e->kind == ExprKind::SExt;
        if (e->kind == ExprKind::SExt && e->to_width > v.width && v.width > 0 && (v.bits & (std::uint64_t{1} << (v.width - 1)))) {
            v.bits |= (~evalMask(v.width));
        }
        return evalMake(v.bits, type);
    }
    case ExprKind::Slice: {
        auto b = evalExpr(e->base, vars, lookup_tables);
        int width = e->hi - e->lo + 1;
        return evalMake((b.bits >> e->lo) & evalMask(width), make_hw_type("UInt", width, false));
    }
    case ExprKind::BitSelect: {
        auto b = evalExpr(e->base, vars, lookup_tables);
        return evalMake((b.bits >> e->bit) & 1, boolean());
    }
    case ExprKind::WriteSlice: {
        auto b = evalExpr(e->base, vars, lookup_tables);
        auto v = evalExpr(e->value, vars, lookup_tables);
        int width = e->hi - e->lo + 1;
        std::uint64_t field = evalMask(width) << e->lo;
        return evalMake((b.bits & ~field) | ((v.bits & evalMask(width)) << e->lo), e->type.width > 0 ? e->type : e->base->type);
    }
    case ExprKind::WriteBit: {
        auto b = evalExpr(e->base, vars, lookup_tables);
        auto v = evalExpr(e->value, vars, lookup_tables);
        std::uint64_t bit = std::uint64_t{1} << e->bit;
        return evalMake(v.bits & 1 ? (b.bits | bit) : (b.bits & ~bit), e->type.width > 0 ? e->type : e->base->type);
    }
    case ExprKind::Concat: {
        std::uint64_t bits = 0;
        int width = 0;
        for (const auto& part : e->parts) {
            auto v = evalExpr(part, vars, lookup_tables);
            bits = (bits << v.width) | (v.bits & evalMask(v.width));
            width += v.width;
        }
        return evalMake(bits, make_hw_type("UInt", width, false));
    }
    case ExprKind::Repeat: {
        auto v = evalExpr(e->operand, vars, lookup_tables);
        std::uint64_t bits = 0;
        for (int i = 0; i < e->times; ++i) bits = (bits << v.width) | (v.bits & evalMask(v.width));
        return evalMake(bits, e->type);
    }
    case ExprKind::ReduceOr: {
        auto v = evalExpr(e->operand, vars, lookup_tables);
        return evalMake((v.bits & evalMask(v.width)) != 0 ? 1 : 0, boolean());
    }
    case ExprKind::ReduceAnd: {
        auto v = evalExpr(e->operand, vars, lookup_tables);
        return evalMake((v.bits & evalMask(v.width)) == evalMask(v.width) ? 1 : 0, boolean());
    }
    case ExprKind::ReduceXor: {
        auto v = evalExpr(e->operand, vars, lookup_tables);
        std::uint64_t bits = v.bits & evalMask(v.width);
        int parity = 0;
        while (bits != 0) {
            parity ^= static_cast<int>(bits & 1);
            bits >>= 1;
        }
        return evalMake(parity, boolean());
    }
    case ExprKind::Call: {
        if (e->callee == "lookup") {
            if (e->args.size() != 2 || !e->args[0] || e->args[0]->kind != ExprKind::Literal) {
                throw std::runtime_error("invalid lookup in Predicate IR evaluator");
            }
            auto table = lookup_tables.find(e->args[0]->literal_value);
            if (table == lookup_tables.end()) throw std::runtime_error("unknown lookup table in Predicate IR evaluator");
            auto idx = evalExpr(e->args[1], vars, lookup_tables).bits;
            if (idx >= table->second.size()) throw std::runtime_error("lookup index out of bounds in Predicate IR evaluator");
            return evalMake(evalParseLiteral(table->second[static_cast<size_t>(idx)]), e->type);
        }
        if (e->intrinsic == IntrinsicKind::DynamicRangeAt || e->callee == "__dynamic_range_at") {
            auto b = evalExpr(e->args[0], vars, lookup_tables);
            auto lo = static_cast<int>(evalExpr(e->args[1], vars, lookup_tables).bits);
            return evalMake((b.bits >> lo) & evalMask(e->type.width), e->type);
        }
        if (e->intrinsic == IntrinsicKind::DynamicBitAt || e->callee == "__dynamic_bit_at") {
            auto b = evalExpr(e->args[0], vars, lookup_tables);
            auto bit = static_cast<int>(evalExpr(e->args[1], vars, lookup_tables).bits);
            return evalMake((b.bits >> bit) & 1, boolean());
        }
        throw std::runtime_error("unlowered call in Predicate IR evaluator: " + e->callee);
    }
    case ExprKind::Cast:
        return evalExpr(e->cast_expr, vars, lookup_tables);
    case ExprKind::ArrayAccess:
    case ExprKind::FieldAccess:
        throw std::runtime_error("unlowered aggregate access in Predicate IR evaluator");
    }
    throw std::runtime_error("unknown expression kind in Predicate IR evaluator");
}

static EvalVars evalProgram(const PredicateProgram& prog, EvalVars vars) {
    for (const auto& output : prog.output_expressions) {
        std::string seed = output.name + "_0";
        if (vars.find(seed) == vars.end()) {
            bool is_bool = output.type.width == 1 || output.type.hw_kind == "bool";
            vars[seed] = evalMake(0, is_bool ? boolean() : output.type);
        }
    }
    for (const auto& assignment : prog.assignments) {
        auto guard = assignment.guard ? evalExpr(assignment.guard, vars, prog.lookup_tables) : evalMake(1, boolean());
        if (guard.bits == 0) continue;
        if (!assignment.target || assignment.target->kind != ExprKind::VarRef) {
            throw std::runtime_error("non-variable assignment target in Predicate IR evaluator");
        }
        vars[assignment.target->var_name] = evalExpr(assignment.value, vars, prog.lookup_tables);
    }
    for (const auto& output : prog.output_expressions) {
        vars[output.name] = evalExpr(output.expr, vars, prog.lookup_tables);
    }
    return vars;
}

void test_if_else_phi_ite() {
    auto ifs = std::make_shared<Stmt>();
    ifs->kind = StmtKind::If;
    ifs->if_cond = make_var("sel", boolean());
    ifs->if_then.push_back(assign("y", make_var("a", u(8)), u(8)));
    ifs->if_else.push_back(assign("y", make_var("b", u(8)), u(8)));

    auto f = fn({decl("y", u(8)), ifs, assign("out", make_var("y", u(8)), u(8))});
    auto r = runPipeline(f);
    CHECK(r.error.empty());
    checkContains(r.text, "ite(");
    checkContains(r.text, "y_");
    checkContains(r.text, "out_");
    checkContains(r.text, "ite(sel_0");
    checkContains(r.text, "y_1");
    checkContains(r.text, "y_2");
    CHECK(!r.prog.output_expressions.empty());
    checkContains(r.text, "output_expressions:");
    std::cout << "  PASS: test_if_else_phi_ite\n";
}

void test_nested_if() {
    auto inner = std::make_shared<Stmt>();
    inner->kind = StmtKind::If;
    inner->if_cond = make_binary("==", make_var("a", u(8)), make_literal("0", u(8)), boolean());
    inner->if_then.push_back(assign("y", make_literal("1", u(8)), u(8)));
    inner->if_else.push_back(assign("y", make_literal("2", u(8)), u(8)));

    auto outer = std::make_shared<Stmt>();
    outer->kind = StmtKind::If;
    outer->if_cond = make_var("sel", boolean());
    outer->if_then.push_back(inner);
    outer->if_else.push_back(assign("y", make_literal("3", u(8)), u(8)));

    auto r = runPipeline(fn({decl("y", u(8)), outer, assign("out", make_var("y", u(8)), u(8))}));
    CHECK(r.error.empty());
    checkContains(r.text, "ite(");
    checkContains(r.text, "&&");
    std::cout << "  PASS: test_nested_if\n";
}

void test_uneven_merge_then_if_guard() {
    auto inner = std::make_shared<Stmt>();
    inner->kind = StmtKind::If;
    inner->if_cond = make_binary("==", make_var("a", u(8)), make_literal("0", u(8)), boolean());
    inner->if_then.push_back(assign("y", make_literal("2", u(8)), u(8)));
    inner->if_else.push_back(assign("y", make_literal("3", u(8)), u(8)));

    auto outer = std::make_shared<Stmt>();
    outer->kind = StmtKind::If;
    outer->if_cond = make_var("sel", boolean());
    outer->if_then.push_back(assign("y", make_literal("1", u(8)), u(8)));
    outer->if_else.push_back(inner);

    auto final_if = std::make_shared<Stmt>();
    final_if->kind = StmtKind::If;
    final_if->if_cond = make_var("sel2", boolean());
    final_if->if_then.push_back(assign("out", make_var("y", u(8)), u(8)));
    final_if->if_else.push_back(assign("out", make_var("a", u(8)), u(8)));

    auto f = fn({decl("y", u(8)), outer, final_if});
    f.params.push_back(param(boolean(), "sel2"));
    auto r = runPipeline(f);
    if (!r.error.empty()) std::cerr << "test_uneven_merge_then_if_guard error: " << r.error << "\n";
    CHECK(r.error.empty());
    checkContains(r.text, "when (sel2_0): out");
    CHECK(r.text.find("when ((sel_0 && sel2_0)): out") == std::string::npos);
    std::cout << "  PASS: test_uneven_merge_then_if_guard\n";
}

void test_switch_case() {
    auto sw = std::make_shared<Stmt>();
    sw->kind = StmtKind::Switch;
    sw->switch_expr = make_var("a", u(8));
    CaseClause c0;
    c0.value = make_literal("0", u(8));
    c0.body.push_back(assign("y", make_var("b", u(8)), u(8)));
    auto c0_break = std::make_shared<Stmt>();
    c0_break->kind = StmtKind::Break;
    c0.body.push_back(c0_break);
    CaseClause def;
    def.value = std::nullopt;
    def.body.push_back(assign("y", make_literal("7", u(8)), u(8)));
    auto def_break = std::make_shared<Stmt>();
    def_break->kind = StmtKind::Break;
    def.body.push_back(def_break);
    sw->switch_cases = {c0, def};

    auto r = runPipeline(fn({decl("y", u(8)), sw, assign("out", make_var("y", u(8)), u(8))}));
    CHECK(r.error.empty());
    checkContains(r.text, "ite(");
    checkContains(r.text, "==");
    std::cout << "  PASS: test_switch_case\n";
}

void test_fixed_for_unroll() {
    auto loop = std::make_shared<Stmt>();
    loop->kind = StmtKind::For;
    loop->for_init = decl("i", TypeInfo{"int", 32, true}, make_literal("0", TypeInfo{"int", 32, true}));
    loop->for_cond = make_binary("<", make_var("i", TypeInfo{"int", 32, true}), make_literal("3", TypeInfo{"int", 32, true}), boolean());
    loop->for_step = make_unary("++", make_var("i", TypeInfo{"int", 32, true}), TypeInfo{"int", 32, true});
    loop->for_body.push_back(assign("sum", make_binary("+", make_var("sum", u(8)), make_literal("1", u(8)), u(8)), u(8)));

    auto r = runPipeline(fn({decl("sum", u(8), make_literal("0", u(8))), loop, assign("out", make_var("sum", u(8)), u(8))}));
    CHECK(r.error.empty());
    CHECK(r.text.find("sum_3") != std::string::npos);
    std::cout << "  PASS: test_fixed_for_unroll\n";
}

void test_for_continue_break_predication() {
    auto loop = std::make_shared<Stmt>();
    loop->kind = StmtKind::For;
    loop->for_init = decl("i", TypeInfo{"int", 32, true}, make_literal("0", TypeInfo{"int", 32, true}));
    loop->for_cond = make_binary("<", make_var("i", TypeInfo{"int", 32, true}), make_literal("4", TypeInfo{"int", 32, true}), boolean());
    loop->for_step = make_unary("++", make_var("i", TypeInfo{"int", 32, true}), TypeInfo{"int", 32, true});

    auto cont_if = std::make_shared<Stmt>();
    cont_if->kind = StmtKind::If;
    cont_if->if_cond = make_binary("==", make_var("i", TypeInfo{"int", 32, true}), make_literal("1", TypeInfo{"int", 32, true}), boolean());
    auto cont = std::make_shared<Stmt>();
    cont->kind = StmtKind::Continue;
    cont_if->if_then.push_back(cont);

    auto break_if = std::make_shared<Stmt>();
    break_if->kind = StmtKind::If;
    break_if->if_cond = make_binary("==", make_var("i", TypeInfo{"int", 32, true}), make_literal("3", TypeInfo{"int", 32, true}), boolean());
    auto brk = std::make_shared<Stmt>();
    brk->kind = StmtKind::Break;
    break_if->if_then.push_back(brk);

    loop->for_body.push_back(cont_if);
    loop->for_body.push_back(break_if);
    loop->for_body.push_back(assign("sum", make_binary("+", make_var("sum", u(8)), make_literal("1", u(8)), u(8)), u(8)));

    auto r = runPipeline(fn({decl("sum", u(8), make_literal("0", u(8))), loop, assign("out", make_var("sum", u(8)), u(8))}));
    CHECK(r.error.empty());
    checkContains(r.text, "__loop_active_0");
    checkContains(r.text, "__loop_active_0_continue_1");
    checkContains(r.text, "ite(");
    std::cout << "  PASS: test_for_continue_break_predication\n";
}

void test_dynamic_for_error() {
    auto loop = std::make_shared<Stmt>();
    loop->kind = StmtKind::For;
    loop->for_init = decl("i", TypeInfo{"int", 32, true}, make_literal("0", TypeInfo{"int", 32, true}));
    loop->for_cond = make_binary("<", make_var("i", TypeInfo{"int", 32, true}), make_var("a", u(8)), boolean());
    loop->for_step = make_unary("++", make_var("i", TypeInfo{"int", 32, true}), TypeInfo{"int", 32, true});
    loop->for_body.push_back(assign("out", make_var("b", u(8)), u(8)));

    auto r = runPipeline(fn({loop}));
    CHECK(!r.error.empty());
    checkContains(r.error, "Cannot statically analyze");
    std::cout << "  PASS: test_dynamic_for_error\n";
}

void test_uninitialized_and_partial_output_errors() {
    auto read = runPipeline(fn({decl("y", u(8)), assign("out", make_var("y", u(8)), u(8))}));
    CHECK(!read.error.empty());
    checkContains(read.error, "uninitialized");

    auto typo = runPipeline(fn({assign("out", make_binary("+", make_var("typo", u(8)), make_literal("1", u(8)), u(9)), u(8))}));
    CHECK(!typo.error.empty());
    checkContains(typo.error, "Unknown variable 'typo'");

    auto maybe_init = std::make_shared<Stmt>();
    maybe_init->kind = StmtKind::If;
    maybe_init->if_cond = make_var("sel", boolean());
    maybe_init->if_then.push_back(assign("x", make_literal("1", u(8)), u(8)));
    auto partial_local = runPipeline(fn({decl("x", u(8)), maybe_init, assign("out", make_var("x", u(8)), u(8))}));
    CHECK(!partial_local.error.empty());
    checkContains(partial_local.error, "uninitialized");

    auto ifs = std::make_shared<Stmt>();
    ifs->kind = StmtKind::If;
    ifs->if_cond = make_var("sel", boolean());
    ifs->if_then.push_back(assign("out", make_var("a", u(8)), u(8)));
    auto partial = runPipeline(fn({ifs}));
    CHECK(partial.error.empty());
    CHECK(partial.prog.param_directions.at("out") == "InOut");
    CHECK(std::find(partial.prog.inputs.begin(), partial.prog.inputs.end(), "out") !=
          partial.prog.inputs.end());
    checkContains(emitListJson(partial.prog), "out_0");
    bool has_inout_diagnostic = false;
    for (const auto& diagnostic : partial.prog.diagnostics) {
        if (diagnostic.find("partial_mutable_reference_promoted_to_inout") !=
            std::string::npos) {
            has_inout_diagnostic = true;
        }
    }
    CHECK(has_inout_diagnostic);

    FunctionAST vld_fn;
    vld_fn.name = "default_vld";
    vld_fn.return_type = TypeInfo{"void", 0, false};
    vld_fn.params.push_back(param(boolean(), "result_valid", ParamDirection::Output));
    auto default_vld = runPipeline(vld_fn);
    CHECK(!default_vld.error.empty());
    checkContains(default_vld.error, "missing_assignment_for_non_defaultable_output");
    std::cout << "  PASS: test_uninitialized_and_partial_output_errors\n";
}

void test_procedure_inline_return_is_callee_local() {
    FunctionAST f;
    f.name = "inline_return";
    f.return_type = TypeInfo{"void", 0, false};
    f.params.push_back(param(boolean(), "c"));
    f.params.push_back(param(u(8), "tail", ParamDirection::Output));

    auto h = std::make_shared<FunctionAST>();
    h->name = "h";
    h->return_type = TypeInfo{"void", 0, false};
    auto early = std::make_shared<Stmt>();
    early->kind = StmtKind::If;
    early->if_cond = make_var("c", boolean());
    auto ret = std::make_shared<Stmt>();
    ret->kind = StmtKind::Return;
    early->if_then.push_back(ret);
    h->body.push_back(early);
    h->body.push_back(assign("out", make_literal("7", u(8)), u(8)));
    f.lambdas["h"] = h;

    auto call = std::make_shared<Stmt>();
    call->kind = StmtKind::ExprStmt;
    call->expr_stmt = std::make_shared<Expr>();
    call->expr_stmt->kind = ExprKind::Call;
    call->expr_stmt->callee = "h";
    call->expr_stmt->type = TypeInfo{"void", 0, false};
    f.body.push_back(decl("out", u(8)));
    f.body.push_back(call);
    f.body.push_back(assign("tail", make_literal("9", u(8)), u(8)));

    auto r = runPipeline(f);
    CHECK(r.error.empty());
    checkContains(r.text, "out");
    checkContains(r.text, "7");
    checkContains(r.text, "c_0");
    checkContains(r.text, "tail");
    checkContains(r.text, "9");
    CHECK(r.text.find("return;") == std::string::npos);
    std::cout << "  PASS: test_procedure_inline_return_is_callee_local\n";
}

void test_array_dynamic_index_flatten() {
    FunctionAST f = fn({});
    TypeInfo arr = u(8);
    arr.is_array = true;
    arr.array_size = 3;
    f.params.push_back(param(arr, "arr"));
    f.body.push_back(assign("out", make_array_access(make_var("arr", arr), make_var("a", u(8)), u(8)), u(8)));

    auto r = runPipeline(f);
    CHECK(r.error.empty());
    checkContains(r.text, "ite((a_0 == 0), arr_0");
    checkContains(r.text, "arr_2");
    std::cout << "  PASS: test_array_dynamic_index_flatten\n";
}

void test_nested_array_flatten() {
    FunctionAST f = fn({});
    TypeInfo arr = u(8);
    arr.is_array = true;
    arr.array_size = 2;
    arr.array_dims = {2, 3};
    f.params.push_back(param(arr, "grid"));
    auto access = make_array_access(
        make_array_access(make_var("grid", arr), make_var("sel", boolean()), arr),
        make_var("a", u(8)),
        u(8));
    f.body.push_back(assign("out", access, u(8)));

    auto r = runPipeline(f);
    CHECK(r.error.empty());
    checkContains(r.text, "grid_0_0");
    checkContains(r.text, "grid_1_2");
    checkContains(r.text, "ite(");
    std::cout << "  PASS: test_nested_array_flatten\n";
}

void test_dynamic_array_write_requires_initialized_array() {
    TypeInfo arr = u(8);
    arr.is_array = true;
    arr.array_size = 2;
    arr.array_dims = {2};

    auto write = std::make_shared<Stmt>();
    write->kind = StmtKind::Assign;
    write->assign_target = make_array_access(make_var("tmp", arr), make_var("a", u(8)), u(8));
    write->assign_value = make_var("b", u(8));

    auto bad = runPipeline(fn({decl("tmp", arr), write, assign("out", make_array_access(make_var("tmp", arr), make_literal("0", u(8)), u(8)), u(8))}));
    CHECK(!bad.error.empty());
    checkContains(bad.error, "requires all elements to be initialized");
    CHECK(bad.text.find("tmp_0_old") == std::string::npos);

    arr.init_values = {"0"};
    auto good_write = std::make_shared<Stmt>(*write);
    good_write->assign_target = make_array_access(make_var("tmp", arr), make_var("a", u(8)), u(8));
    auto good = runPipeline(fn({decl("tmp", arr), good_write, assign("out", make_array_access(make_var("tmp", arr), make_literal("0", u(8)), u(8)), u(8))}));
    CHECK(good.error.empty());
    checkContains(good.text, "tmp_0_0 = 0");
    checkContains(good.text, "tmp_1_0 = 0");
    checkContains(good.text, "ite((a_0 == 0), b_0, tmp_0_0)");
    std::cout << "  PASS: test_dynamic_array_write_requires_initialized_array\n";
}

void test_struct_flatten() {
    TypeInfo st;
    st.name = "Data";
    st.struct_name = "Data";
    FunctionAST f = fn({});
    f.params.push_back(param(st, "data"));
    f.body.push_back(assign("out", make_field_access(make_var("data", st), "a", u(8)), u(8)));

    auto r = runPipeline(f);
    CHECK(r.error.empty());
    checkContains(r.text, "data_a");
    CHECK(r.text.find("data.a") == std::string::npos);
    std::cout << "  PASS: test_struct_flatten\n";
}

void test_uint_width_and_illegal_call() {
    auto r = runPipeline(fn({assign("out", make_binary("*", make_var("a", u(8)), make_var("b", u(8)), u(16)), u(16))}));
    CHECK(r.error.empty());
    checkContains(r.json, "\"width\": 16");

    auto call = std::make_shared<Expr>();
    call->kind = ExprKind::Call;
    call->callee = "vector_push";
    auto bad = runPipeline(fn({assign("out", call, u(8))}));
    CHECK(!bad.error.empty());
    checkContains(bad.error, "Unsupported function call");
    std::cout << "  PASS: test_uint_width_and_illegal_call\n";
}

void test_int_semantics_core_widths() {
    auto add = IntSemantics::binaryResultType("+", u(8), u(16));
    CHECK(add.error.empty());
    CHECK(add.type.width == 17);

    auto sub = IntSemantics::binaryResultType("-", u(8), u(16));
    CHECK(sub.error.empty());
    CHECK(sub.type.width == 16);

    auto mul = IntSemantics::binaryResultType("*", u(8), u(16));
    CHECK(mul.error.empty());
    CHECK(mul.type.width == 24);

    auto sh = IntSemantics::binaryResultType(">>", u(8), TypeInfo{"int", 32, true});
    CHECK(sh.error.empty());
    CHECK(sh.type.width == 8);

    auto cmp = IntSemantics::binaryResultType("<", u(8), u(16));
    CHECK(cmp.error.empty());
    CHECK(cmp.type.width == 1);

    auto bitwise = IntSemantics::binaryResultType("^", u(8), u(16));
    CHECK(!bitwise.error.empty());

    auto div = IntSemantics::binaryResultType("/", u(8), u(8));
    CHECK(!div.error.empty());

    TypeInfo int_bv = make_hw_type("Int", 8, false);
    auto int_add = IntSemantics::binaryResultType("+", int_bv, int_bv);
    CHECK(int_add.error.empty());
    CHECK(int_add.type.width == 9);
    CHECK(int_add.type.hw_kind == "Int");
    CHECK(!int_add.type.is_signed);

    TypeInfo signed_view = int_bv;
    signed_view.hw_kind = "signed_view";
    signed_view.is_signed = true;
    auto signed_add = IntSemantics::binaryResultType("+", signed_view, int_bv);
    CHECK(signed_add.error.empty());
    CHECK(signed_add.type.width == 9);
    CHECK(signed_add.type.hw_kind == "Int");
    CHECK(signed_add.type.is_signed);

    std::cout << "  PASS: test_int_semantics_core_widths\n";
}

void test_power_of_two_div_rem_lowering() {
    auto rem = runPipeline(fn({
        assign("out",
               make_binary("%",
                           make_var("a", u(8)),
                           make_literal("2", TypeInfo{"int", 32, true}),
                           u(8)),
               u(8))
    }));
    CHECK(rem.error.empty());
    checkContains(rem.text, "&");
    CHECK(rem.text.find("%") == std::string::npos);

    auto div = runPipeline(fn({
        assign("out",
               make_binary("/",
                           make_var("a", u(8)),
                           make_literal("4", TypeInfo{"int", 32, true}),
                           u(8)),
               u(8))
    }));
    CHECK(div.error.empty());
    checkContains(div.text, ">>");
    CHECK(div.text.find("/") == std::string::npos);

    auto unsupported = runPipeline(fn({
        assign("out",
               make_binary("%",
                           make_var("a", u(8)),
                           make_literal("3", TypeInfo{"int", 32, true}),
                           u(8)),
               u(8))
    }));
    CHECK(!unsupported.error.empty());
    checkContains(unsupported.error, "Unsupported division/modulo");

    std::cout << "  PASS: test_power_of_two_div_rem_lowering\n";
}

void test_canonical_int_ir_semantics() {
    auto plain_int = canonicalTypeFromTypeInfo(i(8));
    CHECK(plain_int.kind == CanonicalTypeKind::Bits);
    CHECK(plain_int.width == 8);
    CHECK(!plain_int.hasSignedSemantics());
    CHECK(plain_int.str() == "Bits<8,unsigned>");

    auto plain_uint = canonicalTypeFromTypeInfo(u(8));
    CHECK(plain_uint.kind == CanonicalTypeKind::Bits);
    CHECK(!plain_uint.hasSignedSemantics());

    auto signed_view_type = canonicalTypeFromTypeInfo(signedView(8));
    CHECK(signed_view_type.kind == CanonicalTypeKind::SignedView);
    CHECK(signed_view_type.hasSignedSemantics());
    CHECK(signed_view_type.str() == "SignedView(Bits<8>)");

    auto add = canonicalBinary("+", plain_int, canonicalTypeFromTypeInfo(i(16)));
    CHECK(add);
    CHECK(add.op == CanonicalOp::Add);
    CHECK(add.type.width == 17);
    CHECK(!add.type.hasSignedSemantics());
    CHECK(std::string(canonicalOpName(add.op)) == "add");

    auto sub = canonicalBinary("-", plain_int, canonicalTypeFromTypeInfo(u(16)));
    CHECK(sub);
    CHECK(sub.op == CanonicalOp::Sub);
    CHECK(sub.type.width == 16);

    auto mul = canonicalBinary("*", plain_int, canonicalTypeFromTypeInfo(u(16)));
    CHECK(mul);
    CHECK(mul.op == CanonicalOp::Mul);
    CHECK(mul.type.width == 24);
    CHECK(mul.attrs.find("signed") == mul.attrs.end());

    auto signed_mul = canonicalBinary("*", signed_view_type, plain_int);
    CHECK(signed_mul);
    CHECK(signed_mul.op == CanonicalOp::Mul);
    CHECK(signed_mul.type.width == 16);
    CHECK(signed_mul.attrs.at("signed") == "true");

    auto plain_cmp = canonicalBinary("<", plain_int, plain_uint);
    CHECK(plain_cmp);
    CHECK(plain_cmp.op == CanonicalOp::Ult);
    CHECK(plain_cmp.type.kind == CanonicalTypeKind::Bool);

    auto signed_cmp = canonicalBinary("<", signed_view_type, plain_int);
    CHECK(signed_cmp);
    CHECK(signed_cmp.op == CanonicalOp::Slt);
    CHECK(signed_cmp.type.kind == CanonicalTypeKind::Bool);

    auto lshr = canonicalBinary(">>", plain_int, canonicalTypeFromTypeInfo(u(3)));
    CHECK(lshr);
    CHECK(lshr.op == CanonicalOp::LShr);
    CHECK(lshr.type.width == 8);

    auto ashr = canonicalBinary(">>", signed_view_type, canonicalTypeFromTypeInfo(u(3)));
    CHECK(ashr);
    CHECK(ashr.op == CanonicalOp::AShr);
    CHECK(ashr.type.width == 8);

    auto widen_plain = canonicalCast(plain_int, CanonicalType::Bits(16));
    CHECK(widen_plain);
    CHECK(widen_plain.op == CanonicalOp::ZExt);
    auto widen_signed = canonicalCast(signed_view_type, CanonicalType::Bits(16));
    CHECK(widen_signed);
    CHECK(widen_signed.op == CanonicalOp::SExt);
    auto narrow = canonicalCast(plain_int, CanonicalType::Bits(4));
    CHECK(narrow);
    CHECK(narrow.op == CanonicalOp::Trunc);

    auto slice = canonicalSlice(plain_int, 7, 4);
    CHECK(slice);
    CHECK(slice.op == CanonicalOp::Slice);
    CHECK(slice.type.width == 4);

    auto bit = canonicalBitSelect(plain_int, 3);
    CHECK(bit);
    CHECK(bit.op == CanonicalOp::BitSelect);
    CHECK(bit.type.kind == CanonicalTypeKind::Bool);

    auto write_slice = canonicalWriteSlice(plain_int, 3, 0, CanonicalType::Bits(4));
    CHECK(write_slice);
    CHECK(write_slice.op == CanonicalOp::WriteSlice);
    CHECK(write_slice.type.width == 8);

    auto write_bit = canonicalWriteBit(plain_int, 0, CanonicalType::Bool());
    CHECK(write_bit);
    CHECK(write_bit.op == CanonicalOp::WriteBit);

    CanonicalType parts[] = {CanonicalType::Bits(4), CanonicalType::Bits(8), CanonicalType::Bool()};
    auto concat = canonicalConcat(parts, 3);
    CHECK(concat);
    CHECK(concat.op == CanonicalOp::Concat);
    CHECK(concat.type.width == 13);

    auto repeat = canonicalRepeat(CanonicalType::Bits(2), 4);
    CHECK(repeat);
    CHECK(repeat.op == CanonicalOp::Repeat);
    CHECK(repeat.type.width == 8);

    auto reduce = canonicalReduce(CanonicalOp::ReduceXor, plain_int);
    CHECK(reduce);
    CHECK(reduce.op == CanonicalOp::ReduceXor);
    CHECK(reduce.type.kind == CanonicalTypeKind::Bool);

    auto div = canonicalBinary("/", plain_int, plain_int);
    CHECK(!div.error.empty());
    auto bad_bitwise = canonicalBinary("&", CanonicalType::Bits(8), CanonicalType::Bits(16));
    CHECK(!bad_bitwise.error.empty());

    std::cout << "  PASS: test_canonical_int_ir_semantics\n";
}

void test_int_uint_semantics_corner_cases() {
    TypeInfo int8 = i(8);
    CHECK(int8.hw_kind == "Int");
    CHECK(!int8.is_signed);

    auto plain_cmp = IntSemantics::binaryResultType("<", int8, int8);
    CHECK(plain_cmp.error.empty());
    CHECK(plain_cmp.type.width == 1);
    CHECK(!plain_cmp.type.is_signed);

    auto plain_shr = IntSemantics::binaryResultType(">>", int8, u(3));
    CHECK(plain_shr.error.empty());
    CHECK(plain_shr.type.width == 8);
    CHECK(!plain_shr.type.is_signed);
    CHECK(plain_shr.type.hw_kind == "Int");

    auto plain_widen = castIfWidthChanges(make_var("plain", int8), i(16));
    CHECK(plain_widen->kind == ExprKind::ZExt);
    CHECK(plain_widen->to_width == 16);

    TypeInfo sv8 = signedView(8);
    auto signed_cmp = IntSemantics::binaryResultType("<", sv8, int8);
    CHECK(signed_cmp.error.empty());
    CHECK(signed_cmp.type.width == 1);

    auto signed_shr = IntSemantics::binaryResultType(">>", sv8, u(3));
    CHECK(signed_shr.error.empty());
    CHECK(signed_shr.type.width == 8);
    CHECK(signed_shr.type.hw_kind == "signed_view");
    CHECK(signed_shr.type.is_signed);

    auto signed_widen = castIfWidthChanges(make_var("sv", sv8), i(16));
    CHECK(signed_widen->kind == ExprKind::SExt);
    CHECK(signed_widen->to_width == 16);

    auto signed_narrow = castIfWidthChanges(make_var("sv", sv8), i(4));
    CHECK(signed_narrow->kind == ExprKind::WriteBit);
    CHECK(signed_narrow->bit == 3);
    CHECK(signed_narrow->base->kind == ExprKind::Trunc);
    CHECK(signed_narrow->value->kind == ExprKind::BitSelect);
    CHECK(signed_narrow->value->bit == 7);

    auto unsigned_narrow = castIfWidthChanges(make_var("plain", int8), i(4));
    CHECK(unsigned_narrow->kind == ExprKind::Trunc);

    auto bitwise_mismatch = IntSemantics::binaryResultType("&", i(8), u(16));
    CHECK(!bitwise_mismatch.error.empty());

    auto literal_const = make_literal("0xff", u(16));
    CHECK(isWidthCastableConstantExpr(literal_const));
    auto constant_cast = castIfWidthChanges(literal_const, u(8));
    CHECK(constant_cast->kind == ExprKind::Trunc);
    CHECK(constant_cast->to_width == 8);

    auto concat_mix = make_concat({make_var("sv", sv8), make_var("u4", u(4))});
    CHECK(concat_mix->type.width == 12);
    CHECK(concat_mix->type.is_signed);

    auto repeat_signed = make_repeat(make_var("sv", sv8), 2);
    CHECK(repeat_signed->type.width == 16);
    CHECK(repeat_signed->type.is_signed);

    auto trunc_then_zext = castIfWidthChanges(make_trunc(make_var("plain", int8), 4), u(8));
    CHECK(trunc_then_zext->kind == ExprKind::ZExt);
    auto zext_then_trunc = castIfWidthChanges(make_zext(make_var("plain", int8), 16), u(8));
    CHECK(zext_then_trunc->kind == ExprKind::Trunc);

    std::cout << "  PASS: test_int_uint_semantics_corner_cases\n";
}

void test_int_uint_semantics_corner_differential() {
    TypeInfo sv8 = signedView(8);

    auto uint8_neg_one = make_unary("-", make_literal("1", u(8)), u(8));
    CHECK(evalExpr(uint8_neg_one, {}, {}).bits == 0xff);

    auto plain_gt = make_binary(">", make_var("x", i(8)), make_literal("0", i(8)), boolean());
    CHECK(evalExpr(plain_gt, {{"x", evalMake(0x80, i(8))}}, {}).bits == 1);

    auto signed_gt = make_binary(">", make_var("x", sv8), make_literal("0", i(8)), boolean());
    CHECK(evalExpr(signed_gt, {{"x", evalMake(0x80, sv8)}}, {}).bits == 0);

    auto plain_overshift = make_binary(">>", make_var("x", i(8)), make_literal("9", u(4)), i(8));
    CHECK(evalExpr(plain_overshift, {{"x", evalMake(0x80, i(8))}}, {}).bits == 0);

    auto signed_overshift = make_binary(">>", make_var("x", sv8), make_literal("9", u(4)), sv8);
    CHECK(evalExpr(signed_overshift, {{"x", evalMake(0x80, sv8)}}, {}).bits == 0xff);
    CHECK(evalExpr(signed_overshift, {{"x", evalMake(0x7f, sv8)}}, {}).bits == 0);

    auto signed_narrow = castIfWidthChanges(make_var("x", sv8), i(4));
    CHECK(evalExpr(signed_narrow, {{"x", evalMake(0x7f, sv8)}}, {}).bits == 0x7);
    CHECK(evalExpr(signed_narrow, {{"x", evalMake(0x80, sv8)}}, {}).bits == 0x8);
    CHECK(evalExpr(signed_narrow, {{"x", evalMake(0xff, sv8)}}, {}).bits == 0xf);

    auto nested_write = make_write_bit(
        make_write_slice(make_var("word", u(8)), 5, 2, make_literal("0xf", u(4)), u(8)),
        7,
        make_literal("1", boolean()),
        u(8));
    CHECK(evalExpr(nested_write, {{"word", evalMake(0x00, u(8))}}, {}).bits == 0xbc);

    auto low = make_slice(make_var("word", u(8)), 3, 0, u(4));
    auto high = make_slice(make_var("word", u(8)), 7, 4, u(4));
    auto swapped = make_concat({low, high});
    CHECK(evalExpr(swapped, {{"word", evalMake(0xab, u(8))}}, {}).bits == 0xba);

    auto repeated = make_repeat(make_slice(make_var("word", u(8)), 1, 0, u(2)), 4);
    CHECK(evalExpr(repeated, {{"word", evalMake(0x02, u(8))}}, {}).bits == 0xaa);

    auto dyn_range = std::make_shared<Expr>();
    dyn_range->kind = ExprKind::Call;
    dyn_range->callee = "__dynamic_range_at";
    dyn_range->intrinsic = IntrinsicKind::DynamicRangeAt;
    dyn_range->args = {make_var("word", u(8)), make_var("lo", u(3))};
    dyn_range->type = u(4);
    CHECK(evalExpr(dyn_range, {{"word", evalMake(0xf0, u(8))}, {"lo", evalMake(4, u(3))}}, {}).bits == 0xf);

    auto dyn_bit = std::make_shared<Expr>();
    dyn_bit->kind = ExprKind::Call;
    dyn_bit->callee = "__dynamic_bit_at";
    dyn_bit->intrinsic = IntrinsicKind::DynamicBitAt;
    dyn_bit->args = {make_var("word", u(8)), make_var("bit", u(3))};
    dyn_bit->type = boolean();
    CHECK(evalExpr(dyn_bit, {{"word", evalMake(0x80, u(8))}, {"bit", evalMake(7, u(3))}}, {}).bits == 1);

    std::cout << "  PASS: test_int_uint_semantics_corner_differential\n";
}

void test_int_uint_bounds_and_mixed_width_policy() {
    auto mixed_add = IntSemantics::binaryResultType("+", i(8), u(16));
    CHECK(mixed_add.error.empty());
    CHECK(mixed_add.type.width == 17);
    CHECK(mixed_add.type.hw_kind == "Int");
    CHECK(!mixed_add.type.is_signed);

    auto mixed_sub = IntSemantics::binaryResultType("-", i(8), u(16));
    CHECK(mixed_sub.error.empty());
    CHECK(mixed_sub.type.width == 16);

    auto mixed_mul = IntSemantics::binaryResultType("*", i(8), u(16));
    CHECK(mixed_mul.error.empty());
    CHECK(mixed_mul.type.width == 24);

    auto bad_bitwise = IntSemantics::binaryResultType("|", i(8), u(16));
    CHECK(!bad_bitwise.error.empty());

    auto bad_slice = runPipeline(fn({assign("out", make_slice(make_var("a", u(8)), 8, 0, u(9)), u(8))}));
    CHECK(!bad_slice.error.empty());
    checkContains(bad_slice.error, "Slice out of bounds");

    auto bad_bit = runPipeline(fn({assign("out", make_bit_select(make_var("a", u(8)), 8), u(8))}));
    CHECK(!bad_bit.error.empty());
    checkContains(bad_bit.error, "Bit select out of bounds");

    auto bad_range_write = std::make_shared<Stmt>();
    bad_range_write->kind = StmtKind::Assign;
    bad_range_write->assign_target = make_slice(make_var("out", u(8)), 8, 0, u(9));
    bad_range_write->assign_value = make_literal("0", u(9));
    auto bad_range = runPipeline(fn({bad_range_write}));
    CHECK(!bad_range.error.empty());
    checkContains(bad_range.error, "Slice assignment out of bounds");

    auto bad_bit_write = std::make_shared<Stmt>();
    bad_bit_write->kind = StmtKind::Assign;
    bad_bit_write->assign_target = make_bit_select(make_var("out", u(8)), 8);
    bad_bit_write->assign_value = make_literal("1", boolean());
    auto bad_write = runPipeline(fn({bad_bit_write}));
    CHECK(!bad_write.error.empty());
    checkContains(bad_write.error, "Bit assignment out of bounds");

    std::cout << "  PASS: test_int_uint_bounds_and_mixed_width_policy\n";
}

void test_predicate_ir_evaluator_differential_smoke() {
    auto ifs = std::make_shared<Stmt>();
    ifs->kind = StmtKind::If;
    ifs->if_cond = make_binary("<", make_var("a", u(8)), make_var("b", u(8)), boolean());
    auto sum = make_trunc(make_binary("+", make_var("a", u(8)), make_var("b", u(8)), u(9)), 8);
    auto merged = make_write_slice(make_var("b", u(8)), 3, 0, make_slice(make_var("a", u(8)), 3, 0), u(8));
    ifs->if_then.push_back(assign("out", sum, u(8)));
    ifs->if_else.push_back(assign("out", merged, u(8)));

    auto r = runPipeline(fn({ifs}));
    CHECK(r.error.empty());
    CHECK(!r.prog.output_expressions.empty());
    for (std::uint64_t a = 0; a < 16; ++a) {
        for (std::uint64_t b = 0; b < 16; ++b) {
            EvalVars vars;
            vars["a_0"] = evalMake(a, u(8));
            vars["b_0"] = evalMake(b, u(8));
            vars["sel_0"] = evalMake(0, boolean());
            auto out = evalProgram(r.prog, vars).at("out").bits;
            std::uint64_t expected = a < b ? ((a + b) & 0xff) : ((b & 0xf0) | (a & 0x0f));
            CHECK(out == expected);
        }
    }

    PredicateProgram manual;
    manual.lookup_tables["T"] = {"0x03", "0x05", "0x09", "0x0f"};

    auto table_name = make_literal("T", TypeInfo{"table_id", 0, false});
    auto lookup = std::make_shared<Expr>();
    lookup->kind = ExprKind::Call;
    lookup->callee = "lookup";
    lookup->args = {table_name, make_var("idx", u(2))};
    lookup->type = u(4);
    CHECK(evalExpr(lookup, {{"idx", evalMake(2, u(2))}}, manual.lookup_tables).bits == 0x09);

    auto dyn_range = std::make_shared<Expr>();
    dyn_range->kind = ExprKind::Call;
    dyn_range->callee = "__dynamic_range_at";
    dyn_range->intrinsic = IntrinsicKind::DynamicRangeAt;
    dyn_range->args = {make_var("word", u(8)), make_var("lo", u(3))};
    dyn_range->type = u(3);
    CHECK(evalExpr(dyn_range, {{"word", evalMake(0b10110110, u(8))}, {"lo", evalMake(2, u(3))}}, {}).bits == 0b101);

    auto dyn_bit = std::make_shared<Expr>();
    dyn_bit->kind = ExprKind::Call;
    dyn_bit->callee = "__dynamic_bit_at";
    dyn_bit->intrinsic = IntrinsicKind::DynamicBitAt;
    dyn_bit->args = {make_var("word", u(8)), make_var("bit", u(3))};
    dyn_bit->type = boolean();
    CHECK(evalExpr(dyn_bit, {{"word", evalMake(0b10110110, u(8))}, {"bit", evalMake(5, u(3))}}, {}).bits == 1);

    auto concat = make_concat({make_slice(make_var("word", u(8)), 7, 4), make_slice(make_var("word", u(8)), 3, 0)});
    CHECK(evalExpr(concat, {{"word", evalMake(0xab, u(8))}}, {}).bits == 0xab);
    CHECK(evalExpr(make_repeat(make_bit_select(make_var("word", u(8)), 0), 4),
                   {{"word", evalMake(1, u(8))}},
                   {})
              .bits == 0x0f);
    CHECK(evalExpr(make_reduce(ExprKind::ReduceXor, make_var("word", u(4))),
                   {{"word", evalMake(0b1011, u(4))}},
                   {})
              .bits == 1);

    TypeInfo s8 = u(8);
    s8.is_signed = true;
    s8.hw_kind = "signed_view";
    auto signed_lt = make_binary("<", make_var("sa", s8), make_var("sb", s8), boolean());
    CHECK(evalExpr(signed_lt, {{"sa", evalMake(0xff, s8)}, {"sb", evalMake(1, s8)}}, {}).bits == 1);
    auto signed_shr = make_binary(">>", make_var("sa", s8), make_literal("2", u(8)), s8);
    CHECK(evalExpr(signed_shr, {{"sa", evalMake(0xf0, s8)}}, {}).bits == 0xfc);

    auto write_bit = make_write_bit(make_var("word", u(8)), 2, make_literal("1", boolean()), u(8));
    CHECK(evalExpr(write_bit, {{"word", evalMake(0, u(8))}}, {}).bits == 4);
    std::cout << "  PASS: test_predicate_ir_evaluator_differential_smoke\n";
}

void test_int_operator_and_bit_range_differential_smoke() {
    for (std::uint64_t x = 0; x < 256; x += 17) {
        for (std::uint64_t y = 0; y < 256; y += 29) {
            EvalVars vars{{"x", evalMake(x, u(8))}, {"y", evalMake(y, u(8))}};
            auto add = make_binary("+", make_var("x", u(8)), make_var("y", u(8)), u(9));
            CHECK(evalExpr(add, vars, {}).bits == ((x + y) & 0x1ff));
            auto sub = make_binary("-", make_var("x", u(8)), make_var("y", u(8)), u(8));
            CHECK(evalExpr(sub, vars, {}).bits == ((x - y) & 0xff));
            auto mul = make_binary("*", make_var("x", u(8)), make_var("y", u(8)), u(16));
            CHECK(evalExpr(mul, vars, {}).bits == ((x * y) & 0xffff));
            auto trunc = make_trunc(make_binary("+", make_var("x", u(8)), make_literal("300", u(16)), u(16)), 8);
            CHECK(evalExpr(trunc, vars, {}).bits == ((x + 300) & 0xff));

            auto m = make_unary("-", make_binary(">>", make_var("x", u(8)), make_literal("7", u(8)), u(8)), u(8));
            auto xtime = make_binary("^",
                make_binary("<<", make_var("x", u(8)), make_literal("1", u(8)), u(8)),
                make_binary("&", make_literal("0x1b", u(8)), m, u(8)),
                u(8));
            std::uint64_t ref_m = 0 - ((x >> 7) & 1);
            std::uint64_t ref_xtime = ((x << 1) ^ (0x1b & ref_m)) & 0xff;
            CHECK(evalExpr(xtime, vars, {}).bits == ref_xtime);
        }
    }

    TypeInfo s8 = u(8);
    s8.is_signed = true;
    s8.hw_kind = "signed_view";
    CHECK(evalExpr(make_binary("<", make_var("a", s8), make_var("b", s8), boolean()),
                   {{"a", evalMake(0x80, s8)}, {"b", evalMake(0x7f, s8)}},
                   {})
              .bits == 1);
    CHECK(evalExpr(make_binary(">>", make_var("a", s8), make_literal("2", u(8)), s8),
                   {{"a", evalMake(0xf0, s8)}},
                   {})
              .bits == 0xfc);
    CHECK(evalExpr(make_binary(">>", make_var("a", s8), make_literal("9", u(8)), s8),
                   {{"a", evalMake(0x80, s8)}},
                   {})
              .bits == 0xff);

    EvalVars word{{"word", evalMake(0b10110110, u(8))}, {"lo", evalMake(2, u(3))}, {"bit", evalMake(5, u(3))}};
    CHECK(evalExpr(make_write_slice(make_var("word", u(8)), 3, 0, make_literal("0xa", u(4)), u(8)), word, {}).bits == 0xba);
    CHECK(evalExpr(make_write_bit(make_var("word", u(8)), 0, make_literal("1", boolean()), u(8)), word, {}).bits == 0xb7);
    CHECK(evalExpr(make_concat({make_literal("0xa", u(4)), make_literal("0x5", u(4))}), {}, {}).bits == 0xa5);
    CHECK(evalExpr(make_repeat(make_literal("0x3", u(2)), 4), {}, {}).bits == 0xff);
    CHECK(evalExpr(make_reduce(ExprKind::ReduceOr, make_literal("0", u(4))), {}, {}).bits == 0);
    CHECK(evalExpr(make_reduce(ExprKind::ReduceAnd, make_literal("0xf", u(4))), {}, {}).bits == 1);
    CHECK(evalExpr(make_reduce(ExprKind::ReduceXor, make_literal("0xb", u(4))), {}, {}).bits == 1);

    auto dyn_range = std::make_shared<Expr>();
    dyn_range->kind = ExprKind::Call;
    dyn_range->callee = "__dynamic_range_at";
    dyn_range->intrinsic = IntrinsicKind::DynamicRangeAt;
    dyn_range->args = {make_var("word", u(8)), make_var("lo", u(3))};
    dyn_range->type = u(3);
    CHECK(evalExpr(dyn_range, word, {}).bits == 0b101);

    auto dyn_bit = std::make_shared<Expr>();
    dyn_bit->kind = ExprKind::Call;
    dyn_bit->callee = "__dynamic_bit_at";
    dyn_bit->intrinsic = IntrinsicKind::DynamicBitAt;
    dyn_bit->args = {make_var("word", u(8)), make_var("bit", u(3))};
    dyn_bit->type = boolean();
    CHECK(evalExpr(dyn_bit, word, {}).bits == 1);

    std::cout << "  PASS: test_int_operator_and_bit_range_differential_smoke\n";
}

void test_helper_function_inline() {
    auto helper = std::make_shared<FunctionAST>();
    helper->name = "mix";
    helper->return_type = u(8);
    helper->params.push_back(param(u(8), "x"));
    helper->params.push_back(param(u(8), "y"));
    auto ret = std::make_shared<Stmt>();
    ret->kind = StmtKind::Return;
    ret->return_value = make_binary("^", make_var("x", u(8)), make_var("y", u(8)), u(8));
    helper->body.push_back(ret);

    auto call = std::make_shared<Expr>();
    call->kind = ExprKind::Call;
    call->callee = "mix";
    call->args.push_back(make_var("a", u(8)));
    call->args.push_back(make_var("b", u(8)));
    call->type = u(8);

    auto f = fn({assign("out", call, u(8))});
    f.helpers.push_back(helper);
    auto r = runPipeline(f);
    CHECK(r.error.empty());
    checkContains(r.text, "(a_0 ^ b_0)");
    CHECK(r.text.find("mix(") == std::string::npos);
    std::cout << "  PASS: test_helper_function_inline\n";
}

void test_listjson_output_semantics() {
    auto r = runPipeline(fn({assign("out", make_binary("+", make_var("a", u(8)), make_var("b", u(8)), u(8)), u(8))}));
    CHECK(r.error.empty());
    checkContains(r.json, "\"schema_version\": \"rtlzz-signal-list-json-v1\"");
    checkContains(r.json, "\"tool_version\": \"predicate-expand-0.1\"");
    checkContains(r.json, "\"build_commit\"");
    checkContains(r.json, "\"function\": \"comb\"");
    checkContains(r.json, "\"inputs\"");
    checkContains(r.json, "\"outputs\"");
    checkContains(r.json, "\"ports\"");
    checkContains(r.json, "\"signals\"");
    checkContains(r.json, "\"driver\"");
    checkContains(r.json, "\"kind\": \"binary\"");
    checkContains(r.json, "\"text\": \"out");
    std::cout << "  PASS: test_listjson_output_semantics\n";
}

void test_nested_slice_simplification() {
    auto nested = make_slice(
        make_slice(make_var("a", u(8)), 7, 0, u(8)),
        7,
        0,
        u(8));
    auto r = runPipeline(fn({assign("out", nested, u(8))}));
    CHECK(r.error.empty());
    CHECK(r.text.find("slice(slice(") == std::string::npos);
    CHECK(r.json.find("slice(slice(") == std::string::npos);
    checkContains(r.text, "slice(a_0, 7, 0)");
    checkContains(r.json, "\"kind\": \"slice\"");
    std::cout << "  PASS: test_nested_slice_simplification\n";
}

void test_listjson_flattened_driver_semantics() {
    PredicateProgram prog;
    prog.function_name = "listjson_flatten";
    prog.inputs = {"input", "guard"};
    prog.symbols["input"] = u(8);
    prog.symbols["guard"] = boolean();
    prog.symbols["out"] = u(8);
    prog.param_directions["input"] = "Input";
    prog.param_directions["guard"] = "Input";
    prog.param_directions["out"] = "Output";

    prog.assignments.push_back({
        make_literal("true", boolean()),
        make_var("tmp1", u(8)),
        make_var("input", u(8)),
        u(8)
    });
    prog.assignments.push_back({
        make_var("guard", boolean()),
        make_var("tmp2", u(9)),
        make_binary("+", make_var("tmp1", u(8)), make_literal("1", u(8)), u(9)),
        u(9)
    });
    prog.assignments.push_back({
        make_literal("true", boolean()),
        make_var("tmp3", u(8)),
        make_trunc(make_var("tmp2", u(9)), 8),
        u(8)
    });
    prog.assignments.push_back({
        make_literal("true", boolean()),
        make_var("dead", u(8)),
        make_var("dead_in", u(8)),
        u(8)
    });
    prog.output_expressions.push_back({"out", make_var("tmp3", u(8)), u(8)});

    auto json = emitListJson(prog);
    checkContains(json, "\"schema_version\": \"rtlzz-signal-list-json-v1\"");
    checkContains(json, "\"kind\": \"port_read\"");
    checkContains(json, "\"kind\": \"assign\"");
    checkContains(json, "\"kind\": \"binary\"");
    checkContains(json, "\"kind\": \"trunc\"");
    checkContains(json, "\"text\": \"tmp1\"");
    checkContains(json, "\"text\": \"tmp2\"");
    checkContains(json, "\"text\": \"tmp3\"");
    checkContains(json, "\"source_assignment\": 1");
    std::cout << "  PASS: test_listjson_flattened_driver_semantics\n";
}

void test_alias_graph_reference_paths() {
    AliasGraph graph;
    graph.bindReference("ro", "input_payload", true, "const_ref_param");
    auto ro = graph.resolve("ro");
    CHECK(ro.has_value());
    CHECK(ro->canonical_name == "input_payload");
    CHECK(!ro->writable);
    CHECK(ro->kind == AliasKind::ConstRef);

    graph.bindReference("rw", "state", false, "mutable_ref_param");
    auto rw = graph.resolve("rw");
    CHECK(rw.has_value());
    CHECK(rw->writable);
    CHECK(rw->mutability == AliasMutability::Mutable);

    AliasTarget field;
    field.name = "packet_payload_3";
    field.kind = AliasKind::FieldRef;
    field.writable = true;
    field.reason = "constructor_member_initializer";
    graph.bindFieldPath("packet", {"payload", "lane"}, field);
    auto nested = graph.resolvePath("packet", {"payload", "lane"});
    CHECK(nested.has_value());
    CHECK(nested->canonical_name == "packet_payload_3");
    CHECK(nested->field_path.size() == 2);

    AliasTarget elem;
    elem.name = "arr_2";
    elem.kind = AliasKind::MutableRef;
    elem.writable = true;
    graph.bindArrayElement("arr", 2, elem);
    auto element = graph.resolvePath("arr", {}, {2});
    CHECK(element.has_value());
    CHECK(element->canonical_name == "arr_2");
    CHECK(element->index_path.size() == 1);

    graph.bindPointer("outp", "out_value", "pointer_output");
    auto ptr = graph.resolve("outp");
    CHECK(ptr.has_value());
    CHECK(ptr->kind == AliasKind::Pointer);
    CHECK(ptr->writable);

    graph.recordUnsupported("escaping reference");
    CHECK(graph.diagnostics().size() == 1);
    std::cout << "  PASS: test_alias_graph_reference_paths\n";
}

void test_typed_dag_stable_json() {
    TypedDAG dag;
    IRType u8{"bits", 8, false};
    auto a = dag.makeVar("a", u8);
    auto b = dag.makeVar("b", u8);
    auto sum0 = dag.makeNode("add", IRType{"bits", 9, false}, {a, b});
    auto sum1 = dag.makeNode("add", IRType{"bits", 9, false}, {a, b});
    CHECK(sum0.node_id == sum1.node_id);
    CHECK(!dag.hasCycle());
    std::string first = dag.stableJson();
    std::string second = dag.stableJson();
    CHECK(first == second);
    checkContains(first, "\"nodes\"");
    checkContains(first, "\"kind\":\"add\"");
    checkContains(first, "\"width\":9");
    std::cout << "  PASS: test_typed_dag_stable_json\n";
}

void test_predicate_evaluator_wide_bits() {
    PredicateEvaluator evaluator;
    TypeInfo u128 = u(128);
    TypeInfo u64 = u(64);
    TypeInfo s8 = u(8);
    s8.is_signed = true;
    s8.hw_kind = "signed_view";
    CHECK(PredicateEvaluator::fromLiteral("-1", 8).hex() == "0xff");
    CHECK(PredicateEvaluator::fromLiteral("-0x2", 8).hex() == "0xfe");
    evaluator.setVar("wide", PredicateEvaluator::fromLiteral("0x100000000000000000000000000000001", 129));
    auto low = make_slice(make_var("wide", u(129)), 63, 0, u64);
    CHECK(evaluator.eval(low).hex() == "0x1");

    auto high = make_slice(make_var("wide", u(129)), 128, 128, boolean());
    CHECK(evaluator.eval(high).hex() == "0x1");

    auto concat = make_concat({
        make_literal("0xffffffffffffffff", u64),
        make_literal("0x1", u64),
    });
    CHECK(PredicateEvaluator().eval(concat).hex() == "0xffffffffffffffff0000000000000001");

    auto add = make_binary("+",
        make_literal("0xffffffffffffffff", u64),
        make_literal("0x1", u64),
        u(65));
    CHECK(PredicateEvaluator().eval(add).hex() == "0x10000000000000000");

    CHECK(PredicateEvaluator().eval(make_binary("-",
        make_literal("0", u(8)),
        make_literal("1", u(8)),
        u(8))).hex() == "0xff");

    PredicateEvaluator signed_eval;
    signed_eval.setVar("neg", PredicateEvaluator::fromLiteral("0xf0", 8, true));
    signed_eval.setVar("pos", PredicateEvaluator::fromLiteral("0x7", 8, true));
    CHECK(signed_eval.eval(make_binary(">>",
        make_var("neg", s8),
        make_literal("2", u(8)),
        s8)).hex() == "0xfc");
    CHECK(signed_eval.eval(make_binary("<",
        make_var("neg", s8),
        make_var("pos", s8),
        boolean())).hex() == "0x1");
    CHECK(signed_eval.eval(make_binary("*",
        make_var("neg", s8),
        make_literal("2", s8),
        make_hw_type("Int", 16, true))).hex() == "0xffe0");

    CHECK(PredicateEvaluator().eval(make_ternary(
        make_literal("true", boolean()),
        make_literal("0xa", u(4)),
        make_literal("0x5", u(4)),
        u(4))).hex() == "0xa");

    PredicateEvaluator word_eval;
    word_eval.setVar("word", PredicateEvaluator::fromLiteral("0xb6", 8));
    word_eval.setVar("lo", PredicateEvaluator::fromLiteral("2", 3));
    word_eval.setVar("bit", PredicateEvaluator::fromLiteral("5", 3));
    CHECK(word_eval.eval(make_write_slice(
        make_var("word", u(8)), 3, 0, make_literal("0xa", u(4)), u(8))).hex() == "0xba");
    CHECK(word_eval.eval(make_write_bit(
        make_var("word", u(8)), 0, make_literal("1", boolean()), u(8))).hex() == "0xb7");
    CHECK(PredicateEvaluator().eval(make_reduce(ExprKind::ReduceOr, make_literal("0", u(4)))).hex() == "0x0");
    CHECK(PredicateEvaluator().eval(make_reduce(ExprKind::ReduceAnd, make_literal("0xf", u(4)))).hex() == "0x1");
    CHECK(PredicateEvaluator().eval(make_reduce(ExprKind::ReduceXor, make_literal("0xb", u(4)))).hex() == "0x1");

    auto dyn_range = std::make_shared<Expr>();
    dyn_range->kind = ExprKind::Call;
    dyn_range->callee = "__dynamic_range_at";
    dyn_range->intrinsic = IntrinsicKind::DynamicRangeAt;
    dyn_range->args = {make_var("word", u(8)), make_var("lo", u(3))};
    dyn_range->type = u(3);
    CHECK(word_eval.eval(dyn_range).hex() == "0x5");

    auto dyn_bit = std::make_shared<Expr>();
    dyn_bit->kind = ExprKind::Call;
    dyn_bit->callee = "__dynamic_bit_at";
    dyn_bit->intrinsic = IntrinsicKind::DynamicBitAt;
    dyn_bit->args = {make_var("word", u(8)), make_var("bit", u(3))};
    dyn_bit->type = boolean();
    CHECK(word_eval.eval(dyn_bit).hex() == "0x1");

    PredicateEvaluator lookup_eval;
    lookup_eval.setLookupTable("LUT", {
        PredicateEvaluator::fromLiteral("0x11", 8),
        PredicateEvaluator::fromLiteral("0x22", 8),
        PredicateEvaluator::fromLiteral("0x33", 8),
    });
    auto lookup = std::make_shared<Expr>();
    lookup->kind = ExprKind::Call;
    lookup->callee = "lookup";
    lookup->args = {make_literal("LUT", u(8)), make_literal("2", u(2))};
    lookup->type = u(8);
    CHECK(lookup_eval.eval(lookup).hex() == "0x33");
    (void)u128;
    std::cout << "  PASS: test_predicate_evaluator_wide_bits\n";
}

static std::vector<std::uint64_t> expectedConsLine(std::uint32_t cycle_raw,
                                                   std::uint32_t sum_raw,
                                                   std::uint32_t d_raw,
                                                   std::uint32_t recv_vld_raw) {
    TypeInfo u8 = u(8);
    PredicateEvaluator evaluator;
    evaluator.setVar("cycle", PredicateEvaluator::fromUInt64(cycle_raw, 8));
    evaluator.setVar("sum", PredicateEvaluator::fromUInt64(sum_raw, 8));
    evaluator.setVar("d", PredicateEvaluator::fromUInt64(d_raw, 8));
    evaluator.setVar("recv_vld", PredicateEvaluator::fromUInt64(recv_vld_raw != 0 ? 1 : 0, 1));

    auto recv_rdy = make_binary("==",
        make_binary("&", make_var("cycle", u8), make_literal("1", u8), u8),
        make_literal("0", u8),
        boolean());
    auto fire = make_binary("&&", cloneExpr(recv_rdy), make_var("recv_vld", boolean()), boolean());
    auto sum_next = make_trunc(make_binary("+", make_var("sum", u8), make_var("d", u8), u(9)), 8);
    auto cycle_next = make_trunc(make_binary("+", make_var("cycle", u8), make_literal("1", u8), u(9)), 8);

    return {
        evalToU64(evaluator, recv_rdy),
        evalToU64(evaluator, fire),
        evalToU64(evaluator, make_ternary(cloneExpr(fire), make_var("sum", u8), make_literal("0", u8), u8)),
        evalToU64(evaluator, fire),
        evalToU64(evaluator, make_ternary(cloneExpr(fire), sum_next, make_literal("0", u8), u8)),
        1,
        evalToU64(evaluator, cycle_next),
    };
}

static std::vector<std::uint64_t> expectedIntOpsLine(std::uint32_t a_raw,
                                                     std::uint32_t b_raw,
                                                     std::uint32_t sh_raw) {
    TypeInfo u8 = u(8);
    TypeInfo u4 = u(4);
    TypeInfo s8 = signedView(8);
    PredicateEvaluator evaluator;
    evaluator.setVar("a", PredicateEvaluator::fromUInt64(a_raw, 8));
    evaluator.setVar("b", PredicateEvaluator::fromUInt64(b_raw, 8));
    evaluator.setVar("sh", PredicateEvaluator::fromUInt64(sh_raw, 4));
    evaluator.setLookupTable("LUT", {
        PredicateEvaluator::fromUInt64(0x11, 8),
        PredicateEvaluator::fromUInt64(0x22, 8),
        PredicateEvaluator::fromUInt64(0x33, 8),
        PredicateEvaluator::fromUInt64(0x44, 8),
    });

    auto x_shift7 = make_binary(">>", make_var("a", u8), make_literal("7", u8), u8);
    auto m = make_unary("-", x_shift7, u8);
    auto mul2 = make_binary("^",
        make_binary("<<", make_var("a", u8), make_literal("1", u8), u8),
        make_binary("&", make_literal("0x1b", u8), m, u8),
        u8);

    auto dyn_range = std::make_shared<Expr>();
    dyn_range->kind = ExprKind::Call;
    dyn_range->callee = "__dynamic_range_at";
    dyn_range->intrinsic = IntrinsicKind::DynamicRangeAt;
    dyn_range->args = {make_var("a", u8), make_literal(std::to_string(sh_raw % 6U), u(3))};
    dyn_range->type = u(3);

    auto dyn_bit = std::make_shared<Expr>();
    dyn_bit->kind = ExprKind::Call;
    dyn_bit->callee = "__dynamic_bit_at";
    dyn_bit->intrinsic = IntrinsicKind::DynamicBitAt;
    dyn_bit->args = {make_var("a", u8), make_literal(std::to_string(sh_raw % 8U), u(3))};
    dyn_bit->type = boolean();

    auto lookup = std::make_shared<Expr>();
    lookup->kind = ExprKind::Call;
    lookup->callee = "lookup";
    lookup->args = {
        make_literal("LUT", u8),
        make_binary("&", make_var("a", u8), make_literal("3", u8), u8),
    };
    lookup->type = u8;

    return {
        evalToU64(evaluator, make_binary("+", make_var("a", u8), make_var("b", u8), u(9))),
        evalToU64(evaluator, make_binary("-", make_var("a", u8), make_var("b", u8), u8)),
        evalToU64(evaluator, make_binary("*", make_var("a", u8), make_var("b", u8), u(16))),
        evalToU64(evaluator, mul2),
        evalToU64(evaluator, make_binary("<", make_var("a", s8), make_var("b", s8), boolean())),
        evalToU64(evaluator, make_binary(">>", make_var("a", s8), make_var("sh", u4), s8)),
        evalToU64(evaluator, make_binary(">>", make_var("a", u8), make_var("sh", u4), u8)),
        evalToU64(evaluator, make_write_slice(make_var("a", u8), 3, 0, make_var("b", u8), u8)),
        evalToU64(evaluator, make_write_bit(make_var("a", u8), 0, make_binary("&", make_var("b", u8), make_literal("1", u8), boolean()), u8)),
        evalToU64(evaluator, make_concat({make_trunc(make_var("a", u8), 4), make_trunc(make_var("b", u8), 4)})),
        evalToU64(evaluator, make_concat({make_trunc(make_var("a", u8), 2), make_trunc(make_var("b", u8), 3), make_trunc(make_var("sh", u4), 1)})),
        evalToU64(evaluator, make_repeat(make_trunc(make_var("a", u8), 2), 4)),
        evalToU64(evaluator, make_reduce(ExprKind::ReduceOr, make_var("a", u8))),
        evalToU64(evaluator, make_reduce(ExprKind::ReduceAnd, make_var("a", u8))),
        evalToU64(evaluator, make_reduce(ExprKind::ReduceXor, make_var("a", u8))),
        evalToU64(evaluator, dyn_range),
        evalToU64(evaluator, dyn_bit),
        evalToU64(evaluator, lookup),
    };
}

void test_uint_vullib_oracle_differential() {
    auto compiler = findUintOracleCompiler();
    if (compiler.empty()) {
        std::cout << "  SKIP: test_uint_vullib_oracle_differential (clang++ not found)\n";
        return;
    }

    auto root = std::filesystem::current_path();
    auto oracle_src = root / "tests" / "differential" / "uint_semantics" / "uint_oracle.cpp";
    CHECK(std::filesystem::exists(oracle_src));
    auto unique = std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    auto temp = std::filesystem::temp_directory_path() / ("gpef_uint_oracle_" + unique);
    std::filesystem::create_directories(temp);
#ifdef _WIN32
    auto oracle_exe = temp / "uint_oracle.exe";
#else
    auto oracle_exe = temp / "uint_oracle";
#endif
    auto input_path = temp / "input.txt";
    auto output_path = temp / "output.txt";

    std::string compiler_cmd = (compiler.find('\\') != std::string::npos || compiler.find('/') != std::string::npos)
        ? shellQuote(std::filesystem::path(compiler))
        : compiler;
    std::string compile_cmd = compiler_cmd + " -std=c++20 -I" +
        shellQuote(root / "third_party" / "vulsim" / "vullib") + " " +
        shellQuote(oracle_src) + " -o " + shellQuote(oracle_exe);
    CHECK(runShellCommand(compile_cmd) == 0);

    struct Expected {
        std::string mode;
        std::vector<std::uint64_t> values;
    };
    std::vector<Expected> expected;
    {
        std::ofstream input(input_path);
        CHECK(input.good());
        for (int n = 0; n < 4096; ++n) {
            std::uint32_t cycle = static_cast<std::uint32_t>((n * 37 + 11) & 0xff);
            std::uint32_t sum = static_cast<std::uint32_t>((n * 53 + 7) & 0xff);
            std::uint32_t d = static_cast<std::uint32_t>((n * 97 + 3) & 0xff);
            std::uint32_t recv_vld = static_cast<std::uint32_t>((n >> 1) & 1);
            input << "cons " << cycle << ' ' << sum << ' ' << d << ' ' << recv_vld << '\n';
            expected.push_back({"cons", expectedConsLine(cycle, sum, d, recv_vld)});
        }
        for (int n = 0; n < 4096; ++n) {
            std::uint32_t a = static_cast<std::uint32_t>((n * 29 + 0x80) & 0xff);
            std::uint32_t b = static_cast<std::uint32_t>((n * 71 + 0x13) & 0xff);
            std::uint32_t sh = static_cast<std::uint32_t>((n * 5 + 2) & 0xf);
            input << "intops " << a << ' ' << b << ' ' << sh << '\n';
            expected.push_back({"intops", expectedIntOpsLine(a, b, sh)});
        }
    }

    std::string run_cmd = shellQuote(oracle_exe) + " < " + shellQuote(input_path) + " > " + shellQuote(output_path);
    CHECK(runShellCommand(run_cmd) == 0);

    std::ifstream output(output_path);
    CHECK(output.good());
    std::string line;
    size_t index = 0;
    while (std::getline(output, line)) {
        CHECK(index < expected.size());
        std::istringstream is(line);
        std::string mode;
        is >> mode;
        CHECK(mode == expected[index].mode);
        for (size_t i = 0; i < expected[index].values.size(); ++i) {
            std::uint64_t got = 0;
            is >> got;
            if (got != expected[index].values[i]) {
                std::cerr << "Differential mismatch at line " << index
                          << " field " << i << ": got " << got
                          << " expected " << expected[index].values[i]
                          << " line=" << line << "\n";
            }
            CHECK(got == expected[index].values[i]);
        }
        ++index;
    }
    CHECK(index == expected.size());
    output.close();
    std::filesystem::remove_all(temp);
    std::cout << "  PASS: test_uint_vullib_oracle_differential\n";
}

static void runTest(const char* name, void (*fn)()) {
    std::string filter = envValue("GPEF_UNIT_FILTER");
    if (!filter.empty() && std::string(name).find(filter) == std::string::npos) return;
    std::cout << "  RUN: " << name << std::endl;
    fn();
}

int main() {
    std::cout << "Running predicate-expand tests...\n\n";

    runTest("test_if_else_phi_ite", test_if_else_phi_ite);
    runTest("test_nested_if", test_nested_if);
    runTest("test_uneven_merge_then_if_guard", test_uneven_merge_then_if_guard);
    runTest("test_switch_case", test_switch_case);
    runTest("test_fixed_for_unroll", test_fixed_for_unroll);
    runTest("test_for_continue_break_predication", test_for_continue_break_predication);
    runTest("test_dynamic_for_error", test_dynamic_for_error);
    runTest("test_uninitialized_and_partial_output_errors", test_uninitialized_and_partial_output_errors);
    runTest("test_procedure_inline_return_is_callee_local", test_procedure_inline_return_is_callee_local);
    runTest("test_array_dynamic_index_flatten", test_array_dynamic_index_flatten);
    runTest("test_nested_array_flatten", test_nested_array_flatten);
    runTest("test_dynamic_array_write_requires_initialized_array", test_dynamic_array_write_requires_initialized_array);
    runTest("test_struct_flatten", test_struct_flatten);
    runTest("test_uint_width_and_illegal_call", test_uint_width_and_illegal_call);
    runTest("test_int_semantics_core_widths", test_int_semantics_core_widths);
    runTest("test_power_of_two_div_rem_lowering", test_power_of_two_div_rem_lowering);
    runTest("test_canonical_int_ir_semantics", test_canonical_int_ir_semantics);
    runTest("test_int_uint_semantics_corner_cases", test_int_uint_semantics_corner_cases);
    runTest("test_int_uint_semantics_corner_differential", test_int_uint_semantics_corner_differential);
    runTest("test_int_uint_bounds_and_mixed_width_policy", test_int_uint_bounds_and_mixed_width_policy);
    runTest("test_predicate_ir_evaluator_differential_smoke", test_predicate_ir_evaluator_differential_smoke);
    runTest("test_int_operator_and_bit_range_differential_smoke", test_int_operator_and_bit_range_differential_smoke);
    runTest("test_helper_function_inline", test_helper_function_inline);
    runTest("test_listjson_output_semantics", test_listjson_output_semantics);
    runTest("test_nested_slice_simplification", test_nested_slice_simplification);
    runTest("test_listjson_flattened_driver_semantics", test_listjson_flattened_driver_semantics);
    runTest("test_alias_graph_reference_paths", test_alias_graph_reference_paths);
    runTest("test_typed_dag_stable_json", test_typed_dag_stable_json);
    runTest("test_predicate_evaluator_wide_bits", test_predicate_evaluator_wide_bits);
    runTest("test_uint_vullib_oracle_differential", test_uint_vullib_oracle_differential);

    std::cout << "\nAll tests passed.\n";
    return 0;
}
