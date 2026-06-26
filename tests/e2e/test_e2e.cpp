#include "ast/ASTBuilder.h"
#include "ir/CFG.h"
#include "ir/SSA.h"
#include "transform/LoopUnroll.h"
#include "transform/Normalize.h"
#include "transform/Predicate.h"
#include "predicate/OutputExpressionMap.h"
#include "predicate/PredicateVerifier.h"
#include "emitter/TextEmitter.h"
#include "emitter/JsonEmitter.h"
#include "eval/PredicateEvaluator.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

[[noreturn]] static void failCheck(const char* expr, const char* file, int line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expr << "\n";
    std::exit(1);
}

#define CHECK(condition) do { if (!(condition)) failCheck(#condition, __FILE__, __LINE__); } while (false)

struct Result {
    std::optional<pred::PredicateProgram> prog;
    std::string text;
    std::string json;
    std::string error;
};

static Result runFile(const std::string& file, const std::string& top = "hls_main", bool keep_program = false) {
    std::cout << "  RUN file: " << file << " top=" << top << std::endl;
    auto trace = [&](const char* stage) {
        if (std::getenv("GPEF_E2E_TRACE")) std::cerr << "    " << file << ": " << stage << "\n";
    };
    Result r;
    std::vector<std::string> clang_args = {"-I.", "-Ithird_party/vulsim/vullib", "-std=c++20"};
    auto build = pred::buildASTFromSource(file, top, clang_args);
    if (!build.error.empty()) { r.error = build.error; return r; }
    pred::UnrollConfig cfg; cfg.max_iterations = 4096;
    auto unrolled = pred::unrollLoops(build.function->body, cfg);
    if (!unrolled.error.empty()) { r.error = unrolled.error; return r; }
    auto normalized = pred::normalizeFunction(*build.function, unrolled.body);
    if (!normalized.error.empty()) { r.error = normalized.error; return r; }
    auto graph = pred::buildCFG(normalized.body);
    auto ssa = pred::buildSSA(graph, normalized.ssa_seed_symbols);
    if (!ssa.error.empty()) { r.error = ssa.error; return r; }
    auto prog = pred::predicate(ssa);
    prog.function_name = build.function->name;
    prog.symbols = normalized.symbols;
    prog.param_directions = normalized.param_directions;
    prog.output_default_reasons = normalized.output_default_reasons;
    prog.output_paired_controls = normalized.output_paired_controls;
    prog.lookup_tables = normalized.lookup_tables;
    prog.outputs = normalized.output_params;
    if (std::getenv("GPEF_E2E_TRACE")) {
        std::cerr << pred::emitText(prog) << "\n";
    }
    pred::buildOutputExpressionMap(prog);
    auto verified = pred::verifyPredicateProgram(prog);
    if (!verified.ok) { r.error = verified.error; return r; }
    trace("emitText.begin");
    r.text = pred::emitText(prog);
    trace("emitText.end");
    trace("emitJson.begin");
    r.json = pred::emitJson(prog);
    trace("emitJson.end");
    if (keep_program) {
        r.prog = std::move(prog);
    } else {
        // Some stress fixtures intentionally create deep expression trees. The CLI can
        // emit them and exit cleanly, but the e2e harness runs many fixtures in one
        // process and recursive shared_ptr teardown can overflow the test process stack.
        // Keep normal fixture checks focused on emitted text/json and avoid tearing down
        // the temporary IR tree here. Differential tests pass keep_program=true.
        auto* leaked_prog_for_test_process = new pred::PredicateProgram(std::move(prog));
        (void)leaked_prog_for_test_process;
    }
    return r;
}

static void contains(const std::string& s, const std::string& needle) {
    if (s.find(needle) == std::string::npos) std::cerr << "Missing expected text: " << needle << "\n";
    CHECK(s.find(needle) != std::string::npos);
}

static void notContains(const std::string& s, const std::string& needle) {
    if (s.find(needle) != std::string::npos) std::cerr << "Unexpected text: " << needle << "\n";
    CHECK(s.find(needle) == std::string::npos);
}

static void assertNoSourceFallback(const std::string& s) {
    const char* bad[] = {
        "lambda", "auto", "[&]", "setnext(", "output(", "operator()",
        "range_at(", "bit_at(", "repeat(", ".cat(", "Cat(", "helper source"
    };
    for (auto* token : bad) notContains(s, token);
}

static int countJsonArrayValues(const std::string& json, const std::string& name) {
    std::string key = "\"" + name + "\": [";
    size_t start = json.find(key); CHECK(start != std::string::npos); start += key.size();
    size_t end = json.find("]", start); CHECK(end != std::string::npos);
    int count = 0;
    for (size_t pos = start; pos < end; ++pos) {
        if (json[pos] == '"') { ++count; pos = json.find('"', pos + 1); CHECK(pos != std::string::npos && pos < end); }
    }
    return count;
}

static std::string jsonArrayBody(const std::string& json, const std::string& name) {
    std::string key = "\"" + name + "\": [";
    size_t start = json.find(key); CHECK(start != std::string::npos); start += key.size();
    size_t end = json.find("]", start); CHECK(end != std::string::npos);
    return json.substr(start, end - start);
}

static std::string evalTargetName(const pred::ExprPtr& e) {
    if (!e) return "";
    if (e->kind == pred::ExprKind::VarRef) return e->var_name;
    if (e->kind == pred::ExprKind::ArrayAccess) {
        std::string base = evalTargetName(e->array_base);
        if (base.empty() || !e->index || e->index->kind != pred::ExprKind::Literal) return "";
        return base + "_" + e->index->literal_value;
    }
    return "";
}

static bool evalBitsTruthy(const pred::EvalBits& value) {
    for (auto limb : value.limbs) {
        if (limb != 0) return true;
    }
    return false;
}

static void executeProgramAssignments(const pred::PredicateProgram& prog,
                                      pred::PredicateEvaluator& evaluator) {
    for (const auto& assign : prog.assignments) {
        bool active = true;
        if (assign.guard) {
            active = evalBitsTruthy(evaluator.eval(assign.guard));
        }
        if (!active) continue;

        std::string target = evalTargetName(assign.target);
        if (target.empty()) {
            std::cerr << "Differential evaluator unsupported assignment target\n";
            CHECK(false);
        }
        evaluator.setVar(target, evaluator.eval(assign.value));
    }
}

static std::uint64_t evalOutputU64(const pred::PredicateProgram& prog,
                                   const std::string& output,
                                   pred::PredicateEvaluator evaluator) {
    executeProgramAssignments(prog, evaluator);
    for (const auto& expr : prog.output_expressions) {
        if (expr.name == output) {
            auto value = evaluator.eval(expr.expr);
            return value.limbs.empty() ? 0 : value.limbs.front();
        }
    }
    std::cerr << "Missing output expression: " << output << "\n";
    CHECK(false);
    return 0;
}

static void setU16(pred::PredicateEvaluator& evaluator, const std::string& name, std::uint32_t value) {
    evaluator.setVar(name + "_0", pred::PredicateEvaluator::fromUInt64(value & 0xffffu, 16));
}

static void setBool(pred::PredicateEvaluator& evaluator, const std::string& name, bool value) {
    evaluator.setVar(name + "_0", pred::PredicateEvaluator::fromUInt64(value ? 1 : 0, 1));
}

static void checkSa81Differential() {
    auto result = runFile("tests/fixtures/sa8_1.logic.cpp", "hls_main", true);
    if (!result.error.empty()) std::cerr << result.error << "\n";
    CHECK(result.error.empty());
    CHECK(result.prog.has_value());
    notContains(result.json, "UInt<33>");
    notContains(result.json, "\"width\": 33");

    for (std::uint32_t seed = 0; seed < 256; ++seed) {
        std::uint32_t ALeft = (seed * 251u + 7u) & 0xffffu;
        bool AInitLeft = (seed & 1u) != 0;
        std::uint32_t BTop = (seed * 733u + 19u) & 0xffffu;
        std::uint32_t AReg = (seed * 127u + 0x1234u) & 0xffffu;
        bool AInitReg = (seed & 2u) != 0;
        bool BReg = (seed & 4u) != 0;
        std::uint32_t SumReg = (seed * 4099u + 0x55u) & 0xffffu;

        pred::PredicateEvaluator evaluator;
        setU16(evaluator, "ALeft", ALeft);
        setBool(evaluator, "AInitLeft", AInitLeft);
        setU16(evaluator, "BTop", BTop);
        setU16(evaluator, "A_reg_in", AReg);
        setBool(evaluator, "AInit_reg_in", AInitReg);
        setBool(evaluator, "B_reg_in", BReg);
        setU16(evaluator, "Sum_reg_in", SumReg);

        std::uint32_t mulres = (AReg * (BReg ? 1u : 0u)) & 0xffffu;
        std::uint32_t sumres = (mulres + (AInitReg ? 0u : SumReg)) & 0xffffu;

        auto check = [&](const std::string& name, std::uint32_t expected) {
            auto got = evalOutputU64(*result.prog, name, evaluator);
            if (got != expected) {
                std::cerr << "sa8_1 differential mismatch seed=" << seed
                          << " output=" << name
                          << " got=" << got
                          << " expected=" << expected << "\n";
            }
            CHECK(got == expected);
        };
        check("ARight", AReg);
        check("AInitRight", AInitReg ? 1u : 0u);
        check("BBottom", BReg ? 1u : 0u);
        check("CRegOut", SumReg);
        check("A_reg_out", ALeft);
        check("AInit_reg_out", AInitLeft ? 1u : 0u);
        check("B_reg_out", BTop);
        check("Sum_reg_out", sumres);
    }
}

static void checkBranchOutputDifferential() {
    auto result = runFile(
        "tests/fixtures/branch_output_semantics.logic.cpp", "hls_main", true);
    if (!result.error.empty()) std::cerr << result.error << "\n";
    CHECK(result.error.empty());
    CHECK(result.prog.has_value());
    for (std::uint32_t a = 0; a < 16; ++a) {
        for (std::uint32_t b = 0; b < 16; ++b) {
            pred::PredicateEvaluator evaluator;
            evaluator.setVar(
                "a_0", pred::PredicateEvaluator::fromUInt64(a, 8));
            evaluator.setVar(
                "b_0", pred::PredicateEvaluator::fromUInt64(b, 8));
            const std::uint64_t expected =
                a < b ? ((a + b) & 0xffu) : ((b & 0xf0u) | (a & 0x0fu));
            const auto actual =
                evalOutputU64(*result.prog, "out", evaluator);
            CHECK(actual == expected);
        }
    }
}

static void checkQueueProxyConditionalPayload() {
    auto result = runFile(
        "tests/fixtures/queue_proxy_conditional_payload.logic.cpp",
        "hls_main",
        true);
    if (!result.error.empty()) std::cerr << result.error << "\n";
    CHECK(result.error.empty());
    CHECK(result.prog.has_value());
    contains(result.json, "\"q__enqdata__\": \"InOut\"");
    contains(result.json, "\"q__enqvalid__\": \"Output\"");
    notContains(result.json, "\"q__enqdata__\": \"payload_default_zero_when_valid_false\"");
    contains(jsonArrayBody(result.json, "inputs"), "q__enqdata__");

    for (int fire = 0; fire <= 1; ++fire) {
        for (std::uint32_t data : {0u, 1u, 0x5au, 0xffu}) {
            const std::uint32_t old_value = 0xabu;
            pred::PredicateEvaluator evaluator;
            evaluator.setVar(
                "fire_0",
                pred::PredicateEvaluator::fromUInt64(
                    static_cast<std::uint64_t>(fire), 1));
            evaluator.setVar(
                "data_0", pred::PredicateEvaluator::fromUInt64(data, 8));
            evaluator.setVar(
                "q__enqdata___0",
                pred::PredicateEvaluator::fromUInt64(old_value, 8));
            CHECK(evalOutputU64(
                      *result.prog, "q__enqvalid__", evaluator) ==
                  static_cast<std::uint64_t>(fire));
            CHECK(evalOutputU64(
                      *result.prog, "q__enqdata__", evaluator) ==
                  (fire ? data : old_value));
        }
    }
}

static void checkChildResponsePayloadDefault() {
    auto result = runFile(
        "tests/fixtures/child_response_payload_default.logic.cpp",
        "hls_main",
        true);
    if (!result.error.empty()) std::cerr << result.error << "\n";
    CHECK(result.error.empty());
    CHECK(result.prog.has_value());
    contains(result.json, "\"req_out__\": \"Output\"");
    contains(result.json, "\"req_out__\": \"payload_default_zero_when_valid_false\"");
    contains(result.json, "\"paired_control\": \"req__vld__\"");
    notContains(result.json, "partial_mutable_reference_promoted_to_inout: req_out__");

    for (int valid = 0; valid <= 1; ++valid) {
        pred::PredicateEvaluator evaluator;
        evaluator.setVar(
            "req__vld___0",
            pred::PredicateEvaluator::fromUInt64(static_cast<std::uint64_t>(valid), 1));
        CHECK(evalOutputU64(*result.prog, "req_out__", evaluator) ==
              (valid ? 7u : 0u));
    }
}

static void checkFixture(const std::string& path, const std::vector<std::string>& expected) {
    auto result = runFile(path);
    if (!result.error.empty()) std::cerr << result.error << "\n";
    CHECK(result.error.empty());
    contains(result.json, "\"schema_version\": \"gpef-predicate-json-v1\"");
    contains(result.json, "\"tool_version\": \"predicate-expand-0.1\"");
    contains(result.json, "\"build_commit\"");
    contains(result.json, "\"function\": \"hls_main\"");
    contains(result.json, "\"output_expressions\"");
    contains(result.json, "\"param_directions\"");
    for (const auto& item : expected) {
        contains(result.text + "\n" + result.json, item);
    }
    assertNoSourceFallback(result.text);
    assertNoSourceFallback(result.json);
}

static void runInvalidSnippet(const std::string& name, const std::string& code, const std::string& expected) {
    auto dir = std::filesystem::temp_directory_path() / "predicate_expand_generic_tests";
    std::filesystem::create_directories(dir);
    auto path = dir / ("__tmp_" + name + ".cpp");
    { std::ofstream os(path); os << code; }
    auto result = runFile(path.string());
    std::filesystem::remove(path);
    CHECK(!result.error.empty());
    contains(result.error, expected);
}

static Result runSnippet(const std::string& name,
                         const std::string& code,
                         bool keep_program = false) {
    auto dir = std::filesystem::temp_directory_path() / "predicate_expand_generic_tests";
    std::filesystem::create_directories(dir);
    auto path = dir / ("__tmp_" + name + ".cpp");
    { std::ofstream os(path); os << code; }
    auto result = runFile(path.string(), "hls_main", keep_program);
    std::filesystem::remove(path);
    return result;
}

static void checkBuiltinCppSemanticsDifferential() {
    auto result = runSnippet(
        "builtin_cpp_semantics",
        "#include <cstdint>\n"
        "void hls_main(bool sel, int8_t a, uint8_t b,\n"
        "              int16_t& post_inc, int16_t& pre_inc,\n"
        "              int16_t& post_dec, int16_t& pre_dec,\n"
        "              int16_t& add, int16_t& sub, int16_t& ush, int16_t& inv,\n"
        "              int16_t& neg, uint64_t& wrap_add, uint64_t& wrap_mul,\n"
        "              int16_t& choice, bool& cmp8, int32_t c, uint32_t d, bool& cmp32) {\n"
        "  int x = 5; x++; post_inc = x;\n"
        "  int y = 5; ++y; pre_inc = y;\n"
        "  int z = 5; z--; post_dec = z;\n"
        "  int w = 5; --w; pre_dec = w;\n"
        "  add = int8_t(-1) + 0;\n"
        "  sub = uint8_t(0) - 1;\n"
        "  ush = uint8_t(0x80) << 1;\n"
        "  inv = ~uint8_t(0);\n"
        "  neg = -int8_t(-128);\n"
        "  wrap_add = uint32_t(0xffffffffu) + uint32_t(1);\n"
        "  wrap_mul = uint32_t(0xffffffffu) * uint32_t(2);\n"
        "  choice = sel ? a : b;\n"
        "  cmp8 = a < b;\n"
        "  cmp32 = c < d;\n"
        "}\n",
        true);
    if (!result.error.empty()) std::cerr << result.error << "\n";
    CHECK(result.error.empty());
    CHECK(result.prog.has_value());

    pred::PredicateEvaluator evaluator;
    evaluator.setVar("sel_0", pred::PredicateEvaluator::fromUInt64(0, 1));
    evaluator.setVar("a_0", pred::PredicateEvaluator::fromUInt64(0xff, 8, true));
    evaluator.setVar("b_0", pred::PredicateEvaluator::fromUInt64(200, 8));
    evaluator.setVar("c_0", pred::PredicateEvaluator::fromUInt64(0xffffffffu, 32, true));
    evaluator.setVar("d_0", pred::PredicateEvaluator::fromUInt64(1, 32));

    auto check = [&](const std::string& name, std::uint64_t expected) {
        const auto actual = evalOutputU64(*result.prog, name, evaluator);
        if (actual != expected) {
            std::cerr << "builtin differential mismatch output=" << name
                      << " actual=" << actual << " expected=" << expected << "\n";
        }
        CHECK(actual == expected);
    };
    check("post_inc", 6);
    check("pre_inc", 6);
    check("post_dec", 4);
    check("pre_dec", 4);
    check("add", 0xffff);
    check("sub", 0xffff);
    check("ush", 0x0100);
    check("inv", 0xffff);
    check("neg", 0x0080);
    check("wrap_add", 0);
    check("wrap_mul", 0xfffffffeu);
    check("choice", 0x00c8);
    check("cmp8", 1);
    check("cmp32", 0);
}

int main() {
    std::cout << "Running generic predicate-expand e2e tests...\n";

    checkFixture("tests/fixtures/int_basic.logic.cpp", {"output_expressions", "\"signed\": true", "\"width\": 17"});
    checkFixture("tests/fixtures/int_uint_corner.logic.cpp", {
        "\"kind\": \"sext\"",
        "\"kind\": \"zext\"",
        "\"kind\": \"trunc\"",
        "\"kind\": \"write_bit\"",
        "\"kind\": \"write_slice\"",
        "\"kind\": \"concat\"",
        "\"kind\": \"repeat\"",
        "\"signed_shift\"",
        "\"plain_cmp\"",
    });
    checkFixture("tests/fixtures/int_range.logic.cpp", {"\"kind\": \"slice\"", "\"kind\": \"write_slice\"", "\"kind\": \"write_bit\""});
    checkFixture("tests/fixtures/int_concat_repeat.logic.cpp", {"\"kind\": \"concat\"", "\"kind\": \"repeat\"", "\"kind\": \"reduce_or\"", "\"kind\": \"reduce_and\"", "\"kind\": \"reduce_xor\""});
    auto struct_ref = runFile("tests/fixtures/struct_ref.logic.cpp");
    if (!struct_ref.error.empty()) std::cerr << struct_ref.error << "\n";
    CHECK(struct_ref.error.empty());
    contains(struct_ref.json, "\"outputs\": [\"out_x\", \"out_v\", \"copied\"]");
    contains(struct_ref.json, "\"out_x\": \"Output\"");
    contains(struct_ref.json, "\"out_v\": \"Output\"");
    contains(struct_ref.json, "\"target\": \"out_x_");
    contains(struct_ref.json, "\"target\": \"out_v_");
    notContains(struct_ref.json, "\"inputs\": [\"input\", \"pair\"");
    notContains(struct_ref.json, "pair_x");
    notContains(struct_ref.json, "pair_v");
    assertNoSourceFallback(struct_ref.text);
    assertNoSourceFallback(struct_ref.json);
    checkFixture("tests/fixtures/array_flatten.logic.cpp", {"out_3", "\"selected\""});
    checkFixture("tests/fixtures/helper_inline.logic.cpp", {"\"tail\"", "\"out\""});
    checkFixture("tests/fixtures/helper_return_complex.logic.cpp", {"out_x", "out_y", "\"tail\"", "arr_0", "arr_1"});
    checkFixture("tests/fixtures/lambda_inline.logic.cpp", {"\"out\""});
    checkFixture("tests/fixtures/control_predication.logic.cpp", {"ite(", "\"out\""});
    checkBranchOutputDifferential();
    checkQueueProxyConditionalPayload();
    checkChildResponsePayloadDefault();
    auto constant_width = runFile("tests/fixtures/constant_width_context.logic.cpp");
    if (!constant_width.error.empty()) std::cerr << constant_width.error << "\n";
    CHECK(constant_width.error.empty());
    notContains(constant_width.json, "UInt<33>");
    notContains(constant_width.json, "\"width\": 33");
    // These are builtin uint8_t/uint16_t expressions. C++ integral
    // promotion evaluates them as 32-bit int before assignment truncation.
    contains(constant_width.json, "\"type\": \"int\"");
    contains(constant_width.json, "\"width\": 32");
    assertNoSourceFallback(constant_width.text);
    assertNoSourceFallback(constant_width.json);
    auto lookup = runFile("tests/fixtures/lookup_table.logic.cpp");
    if (!lookup.error.empty()) std::cerr << lookup.error << "\n";
    CHECK(lookup.error.empty());
    contains(lookup.json, "\"LUT4\"");
    CHECK(countJsonArrayValues(lookup.json, "LUT4") == 4);
    contains(lookup.text + lookup.json, "lookup(LUT4");
    assertNoSourceFallback(lookup.text);
    assertNoSourceFallback(lookup.json);
    checkFixture("tests/fixtures/default_totalization.logic.cpp", {"\"valid\"", "\"payload\"", "\"has_default_policy\"", "\"default_applied\"", "default_reason"});
    checkFixture("tests/fixtures/dynamic_bounds.logic.cpp", {"__dynamic_bit_at"});
    checkFixture("tests/fixtures/param_directions.logic.cpp", {"\"InOut\"", "\"Output\""});
    checkFixture("tests/fixtures/output_param_inout.logic.cpp", {
        "\"out\": \"InOut\"",
        "\"output_expressions\"",
    });
    checkFixture("tests/fixtures/output_ref_direction.logic.cpp", {
        "\"output__vld__\": \"Output\"",
        "\"output_s__\": \"Output\"",
        "\"output__vld__\"",
        "\"output_s__"
    });
    auto wen_default = runFile("tests/fixtures/write_enable_default_false.logic.cpp");
    if (!wen_default.error.empty()) std::cerr << wen_default.error << "\n";
    CHECK(wen_default.error.empty());
    contains(wen_default.json, "\"wen_sum__\": \"Output\"");
    contains(wen_default.json, "\"wen_sum__\": \"write_enable_default_false\"");
    contains(wen_default.json, "\"wdata_sum__\": \"wdata_default_zero_when_wen_false\"");
    contains(wen_default.text + wen_default.json, "default_applied: wen_sum__");
    contains(wen_default.text + wen_default.json, "default_applied: wen_sum__ default_value=false");
    contains(wen_default.text + wen_default.json, "default_applied: wdata_sum__ default_value=0");
    contains(wen_default.json, "\"assignment_coverage\": \"guarded\"");
    contains(wen_default.json, "\"default_applied\": true");
    contains(wen_default.text + wen_default.json, "false");
    auto output_expr_pos = wen_default.json.find("\"output_expressions\"");
    CHECK(output_expr_pos != std::string::npos);
    notContains(wen_default.json.substr(output_expr_pos), "wen_sum___0");
    auto regproxy_dyn = runFile("tests/fixtures/regproxy_dynamic_array_index.logic.cpp");
    if (!regproxy_dyn.error.empty()) std::cerr << regproxy_dyn.error << "\n";
    CHECK(regproxy_dyn.error.empty());
    contains(regproxy_dyn.json, "\"rdata_regs___0\"");
    contains(regproxy_dyn.json, "\"wen_regs___0\"");
    contains(regproxy_dyn.json, "\"wdata_regs___0\"");
    contains(regproxy_dyn.json, "\"write_enable_default_false\"");
    contains(regproxy_dyn.json, "\"wdata_default_zero_when_wen_false\"");
    contains(regproxy_dyn.json, "\"kind\": \"ite\"");
    notContains(regproxy_dyn.text + regproxy_dyn.json, "array_access");
    notContains(regproxy_dyn.text + regproxy_dyn.json, "operator[]");
    auto semantic_default = runFile("tests/fixtures/semantic_default_policy.logic.cpp");
    if (!semantic_default.error.empty()) std::cerr << semantic_default.error << "\n";
    CHECK(semantic_default.error.empty());
    contains(semantic_default.json, "\"commit_port__\": \"write_enable_default_false\"");
    contains(semantic_default.json, "\"data_port__\": \"commit_port__\"");
    contains(semantic_default.json, "\"paired_control\": \"commit_port__\"");
    contains(semantic_default.json, "\"inactive_semantics\": \"disabled_data\"");
    contains(semantic_default.json, "\"has_default_policy\": true");
    notContains(semantic_default.json, "\"has_default\"");
    notContains(semantic_default.json, "\"default_totalized\"");
    contains(semantic_default.json, "\"commit_port__\": \"Output\"");
    auto semantic_output_expr_pos = semantic_default.json.find("\"output_expressions\"");
    CHECK(semantic_output_expr_pos != std::string::npos);
    notContains(semantic_default.json.substr(semantic_output_expr_pos), "commit_port___0");
    auto reqhelper_policy = runFile("tests/fixtures/reqhelper_output_policy.logic.cpp");
    if (!reqhelper_policy.error.empty()) std::cerr << reqhelper_policy.error << "\n";
    CHECK(reqhelper_policy.error.empty());
    contains(reqhelper_policy.json, "\"output__vld__\": \"valid_default_false\"");
    contains(reqhelper_policy.json, "\"output_s__\": \"output__vld__\"");
    contains(reqhelper_policy.json, "\"paired_control\": \"output__vld__\"");
    contains(reqhelper_policy.json, "\"inactive_value\": \"0\"");
    contains(reqhelper_policy.json, "\"inactive_semantics\": \"disabled_data\"");
    contains(reqhelper_policy.json, "\"default_reason\": \"valid_default_false\"");
    contains(reqhelper_policy.json, "\"default_applied\": false");
    contains(reqhelper_policy.json, "\"assignment_coverage\": \"explicit_initialization_plus_guarded_update\"");
    contains(reqhelper_policy.text, "default-policy=valid_default_false");
    checkFixture("tests/fixtures/symbol_category.logic.cpp", {"\"out\""});
    checkSa81Differential();
    checkBuiltinCppSemanticsDifferential();
    auto prefix_collision = runFile("tests/fixtures/output_prefix_collision.logic.cpp");
    if (!prefix_collision.error.empty()) std::cerr << prefix_collision.error << "\n";
    CHECK(prefix_collision.error.empty());
    CHECK(countJsonArrayValues(prefix_collision.json, "outputs") == 8);
    auto prefix_outputs = jsonArrayBody(prefix_collision.json, "outputs");
    contains(prefix_outputs, "C_top_0");
    contains(prefix_outputs, "C_top_7");
    notContains(prefix_outputs, "C_top_tmp");
    assertNoSourceFallback(prefix_collision.text);
    assertNoSourceFallback(prefix_collision.json);

    runInvalidSnippet("division", "#include <uint.hpp>\nvoid hls_main(Int<8> a, Int<8> b, Int<8>& out) { out = a / b; }\n", "division");
    runInvalidSnippet("pointer_arithmetic", "#include <cstdint>\nvoid hls_main(uint8_t* p, uint8_t& out) { uint8_t* q = p + 1; out = *q; }\n", "pointer arithmetic");
    runInvalidSnippet("helper_new", "#include <uint.hpp>\nInt<8> helper(Int<8> x) { auto* p = new Int<8>(); return x; }\nvoid hls_main(Int<8> x, Int<8>& out) { out = helper(x); }\n", "new");
    runInvalidSnippet("helper_dynamic_loop", "#include <uint.hpp>\nInt<8> helper(Int<8> x, Int<8> n) { Int<8> y = x; for (uint32_t i = 0; i < n; ++i) { y = y + Int<8>(1); } return y; }\nvoid hls_main(Int<8> x, Int<8> n, Int<8>& out) { out = helper(x, n); }\n", "Failed to unroll helper 'helper'");
    runInvalidSnippet("helper_direct_recursion", "#include <uint.hpp>\nInt<8> helper(Int<8> x) { return helper(x); }\nvoid hls_main(Int<8> x, Int<8>& out) { out = helper(x); }\n", "recursion");
    runInvalidSnippet("helper_indirect_recursion", "#include <uint.hpp>\nInt<8> a(Int<8> x);\nInt<8> b(Int<8> x) { return a(x); }\nInt<8> a(Int<8> x) { return b(x); }\nvoid hls_main(Int<8> x, Int<8>& out) { out = a(x); }\n", "recursive helper call cycle");
    runInvalidSnippet("missing_output", "#include <uint.hpp>\nvoid hls_main(Int<8>& out) { }\n", "missing_assignment_for_non_defaultable_output");
    {
        auto invalid_count = runFile("tests/fixtures/negative/name_heuristic_invalid_count_error.logic.cpp");
        CHECK(invalid_count.error.empty());
        contains(invalid_count.json, "\"invalid_count\": \"InOut\"");
        contains(invalid_count.json, "partial_mutable_reference_promoted_to_inout");
        notContains(invalid_count.json, "valid_default_false_by_name_heuristic");
    }
    {
        auto local_uninit = runFile("tests/fixtures/negative/local_uninitialized_read_error.logic.cpp");
        CHECK(!local_uninit.error.empty());
        contains(local_uninit.error, "uninitialized");
    }
    runInvalidSnippet("ready_without_assignment", "#include <uint.hpp>\nvoid hls_main(bool& ready) { }\n", "missing_assignment_for_non_defaultable_output");
    runInvalidSnippet("nested_increment_value",
        "#include <cstdint>\nvoid hls_main(int& out) { int x = 1; out = x++; }\n",
        "increment/decrement in value expression");

    std::cout << "Generic e2e tests passed.\n";
    return 0;
}
