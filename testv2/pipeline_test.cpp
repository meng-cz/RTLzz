#include "backend/pipeline.hpp"
#include "backend/rtlgen.hpp"
#include "rtlzz.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <limits>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; std::exit(1); } } while (false)
using namespace pred;
using namespace beir;

static std::string joinCodeLines(const std::vector<std::string>& lines) {
    std::string result;
    for (const auto& line : lines) result += line;
    return result;
}

static Operand ref(const Program& p, NodeId id) {
    Operand v; v.kind = OperandKind::Symbol; v.node = id; v.type = p.signal(id).type; return v;
}

static NodeId node(Program& p, const std::string& name, int width,
                   OperationKind kind, std::vector<Operand> operands, OpCode op = OpCode::None) {
    Signal s; s.id = p.signals.size(); s.name = name; s.type.width = width;
    Operation d; d.kind = kind; d.op = op; d.type = s.type; d.operands = std::move(operands);
    s.driver = std::move(d); p.signals.push_back(s); return s.id;
}

static NodeId input(Program& p, const std::string& name, int width) {
    Operand v; v.kind = OperandKind::Port; v.text = name; v.type.width = width;
    auto id = node(p, name, width, OperationKind::PortRead, {v});
    p.signal(id).port_name = name;
    Port port; port.name = name; port.type.width = width; port.direction = PortDirection::Input;
    port.element_nodes = {id}; p.ports.push_back(port); p.inputs.push_back(name); return id;
}

static void output(Program& p, const std::string& name, NodeId source) {
    int width = p.signal(source).type.width;
    auto id = node(p, name, width, OperationKind::Assign, {ref(p, source)});
    p.signal(id).port_name = name;
    Port port; port.name = name; port.type.width = width; port.direction = PortDirection::Output;
    port.element_nodes = {id}; p.ports.push_back(port); p.outputs.push_back(name);
}

static Program tradeoff() {
    Program p; p.function_name = "tradeoff";
    auto a = input(p, "a", 1);
    auto u = node(p, "u", 1, OperationKind::Unary, {ref(p, a)}, OpCode::LogicNot);
    auto v = node(p, "v", 64, OperationKind::ZExt, {ref(p, u)});
    auto w = node(p, "w", 1, OperationKind::ReduceOr, {ref(p, v)});
    output(p, "y", w); return p;
}

static void testTradeoff() {
    auto p = tradeoff();
    pipeline::Options o; o.register_stages = 1; o.timing_relaxation_ratio = 0;
    o.operation_delay_weights[rtlzz::PipelineOperation::ReduceOr] = 1.1;
    auto strict = pipeline::build(p, o);
    CHECK(std::abs(strict.report.baseline_delay - 2) < 1e-12);
    CHECK(strict.report.register_bits == 64);
    o.timing_relaxation_ratio = 0.05;
    auto relaxed = pipeline::build(p, o);
    CHECK(relaxed.report.register_bits == 1);
    CHECK(relaxed.report.final_delay <= relaxed.report.allowed_delay);
    CHECK(std::abs(relaxed.report.final_delay - 2.1) < 1e-12);
    CHECK(relaxed.report.baseline_register_bits == 64);
    for (double allowance : {0.1, 0.5, 1.0}) {
        o.timing_relaxation_ratio = allowance;
        auto wider = pipeline::build(p, o);
        CHECK(wider.report.register_bits <= relaxed.report.register_bits);
        CHECK(wider.report.final_delay <= wider.report.allowed_delay);
    }
    o.timing_relaxation_ratio = 0.05;
    CHECK(pipeline::emitText(relaxed) == pipeline::emitText(pipeline::build(p, o)));
    o.timing_relaxation_ratio = 0;
    o.operation_delay_weights[rtlzz::PipelineOperation::LogicalNot] = 4;
    auto weighted = pipeline::build(p, o);
    CHECK(weighted.stages[2] != strict.stages[2]);
    o.operation_delay_weights[rtlzz::PipelineOperation::LogicalNot] = 0;
    CHECK(std::isfinite(pipeline::build(p, o).report.final_delay));
    o.operation_delay_weights[rtlzz::PipelineOperation::ZeroExtend] = 0;
    o.operation_delay_weights[rtlzz::PipelineOperation::ReduceOr] = 0;
    auto zero_cost = pipeline::build(p, o);
    CHECK(zero_cost.report.final_delay == 0);
    CHECK(zero_cost.report.register_bits == 1);
}

static void testAlignment() {
    Program p; p.function_name = "align";
    auto a = input(p, "a", 8), b = input(p, "b", 8);
    auto u = node(p, "u", 8, OperationKind::Binary, {ref(p, a), ref(p, b)}, OpCode::Add);
    auto v = node(p, "v", 8, OperationKind::Binary, {ref(p, u), ref(p, b)}, OpCode::Mul);
    auto w = node(p, "w", 8, OperationKind::Binary, {ref(p, v), ref(p, u)}, OpCode::Add);
    output(p, "long_path", w); output(p, "short_path", a); output(p, "shared_path", a);
    for (int n : {1, 2, 4, 8}) {
        pipeline::Options o; o.register_stages = n;
        auto r = pipeline::build(p, o);
        CHECK(r.report.stage_delays.size() == static_cast<std::size_t>(n + 1));
        CHECK(r.report.final_delay <= r.report.allowed_delay);
        CHECK(r.stages[4] <= n);
        CHECK(r.report.baseline_delay == (n == 1 ? 4 : 3));
        auto short_node = r.rtl_program.signal(6).driver->operands[0].node;
        auto shared_node = r.rtl_program.signal(7).driver->operands[0].node;
        CHECK(short_node == shared_node);
        int captures = 0;
        while (short_node != a) {
            auto it = std::find_if(r.registers.begin(), r.registers.end(), [short_node](const auto& reg) { return reg.node == short_node; });
            CHECK(it != r.registers.end()); short_node = it->source; ++captures;
        }
        CHECK(captures == n);
        auto text = rtlgen::emitSystemVerilog(r);
        CHECK(text.find("always_ff @(posedge clk)") != std::string::npos);
        CHECK(text.find("assign short_path =") != std::string::npos);
        CHECK(text.find("short_path <=") == std::string::npos);
        for (const auto& reg : r.registers)
            CHECK(text.find("assign " + r.rtl_program.signal(reg.node).name + " =") == std::string::npos);
    }
    pipeline::Options zero;
    CHECK(rtlgen::emitSystemVerilog(p) == rtlgen::emitSystemVerilog(pipeline::build(p, zero)));
    zero.register_stages = 1;
    auto broken = pipeline::build(p, zero);
    broken.registers.front().stage += 1;
    bool rejected = false;
    try { pipeline::validateProgram(broken); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
}

static rtlzz::CompileOptions sourceOptions() {
    rtlzz::CompileOptions o;
    o.source_codelines = {"#include <array>\n#include <fixint.hpp>\nInt<8> a;\n#pragma input_port a\nstd::array<Int<8>, 2> y;\n#pragma output_port y\nvoid top() { y[0] = a + Int<8>(1); y[1] = a; }"};
    o.top_function = "top"; o.vullib_dir = "third_party/vulsim/vullib";
    return o;
}

static void testApiAndErrors() {
    auto o = sourceOptions();
    auto combo = rtlzz::compileToRtl(o); if (!combo.ok()) std::cerr << combo.error << "\n"; CHECK(combo.ok());
    o.register_stages = 0;
    CHECK(rtlzz::compileToRtl(o).output_codelines == combo.output_codelines);
    o.register_stages = 2;
    auto piped = rtlzz::compileToRtl(o);
    if (!piped.ok()) std::cerr << piped.error << "\n";
    CHECK(piped.ok()); CHECK(piped.pipeline_report);
    // Verify all direct public configuration fields reach the internal wrapper.
    o.clock_name = "api_clk";
    o.operation_delay_weights[rtlzz::PipelineOperation::Add] = 7;
    o.timing_relaxation_ratio = 0.25;
    auto weighted = rtlzz::compileToRtl(o); CHECK(weighted.ok());
    CHECK(weighted.pipeline_report->baseline_delay == 7);
    CHECK(weighted.pipeline_report->allowed_delay == 8.75);
    CHECK(joinCodeLines(weighted.output_codelines).find("posedge api_clk") != std::string::npos);
    o.clock_name = "clk";
    o.operation_delay_weights.clear(); o.timing_relaxation_ratio = 0.05;
    auto metadata = rtlzz::compileToPortMetadata(o); CHECK(metadata.ok());
    CHECK(joinCodeLines(metadata.output_codelines).find("\"external_capture_cycles\": 3") != std::string::npos);
    o.rtl_module_body = true;
    o.rtl_port_bindings = {{"a", "external_a"}, {"y", "external_y"}, {"clk", "external_clk"}};
    auto body = rtlzz::compileToRtl(o); CHECK(body.ok());
    CHECK(joinCodeLines(body.output_codelines).find("assign clk = external_clk") != std::string::npos);
    o.rtl_port_bindings.pop_back(); CHECK(!rtlzz::compileToRtl(o).ok());
    o.rtl_module_body = false; o.use_circt = true;
    CHECK(rtlzz::compileToRtl(o).error.find("CIRCT") != std::string::npos);
    o.use_circt = false;
    for (const auto& name : {"a", "always_ff", "table", "unique", "bad-clock", ""}) {
        o.clock_name = name; CHECK(!rtlzz::compileToRtl(o).ok());
    }
    o.clock_name = "clk";
    o.register_stages = -1; CHECK(!rtlzz::compileToPortMetadata(o).ok());
    o.register_stages = 1;
    for (double invalid : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        o.operation_delay_weights[rtlzz::PipelineOperation::Add] = invalid;
        CHECK(!rtlzz::compileToRtl(o).ok());
    }
    o.operation_delay_weights.clear(); o.timing_relaxation_ratio = -0.1;
    CHECK(!rtlzz::compileToRtl(o).ok());
}

// Optional artifact for the regression script: the API's module body inserted
// into a containing module, with an external clock binding and changing inputs.
static void emitBodyTestbench(const std::string& path) {
    auto o = sourceOptions();
    o.register_stages = 2;
    o.rtl_module_body = true;
    o.rtl_port_bindings = {{"a", "external_a"}, {"y", "external_y"}, {"clk", "external_clk"}};
    auto result = rtlzz::compileToRtl(o); CHECK(result.ok());
    std::ofstream out(path); CHECK(out.good());
    out << "`timescale 1ns/1ps\nmodule body_testbench;\n"
        << "logic external_clk; logic [7:0] external_a; logic [7:0] external_y [0:1];\n"
        << joinCodeLines(result.output_codelines)
        << R"(
initial begin
    external_clk = 0; external_a = 0;
    #1; external_clk = 1;
    #1; external_clk = 0; external_a = 11;
    #1; external_clk = 1;
    #1; external_clk = 0; external_a = 77;
    #1; external_clk = 1;
    #1; external_clk = 0; external_a = 193;
    #1;
    if (external_y[0] !== 12 || external_y[1] !== 11) $fatal(1, "body first transaction");
    external_clk = 1;
    #1; external_clk = 0;
    #1;
    if (external_y[0] !== 78 || external_y[1] !== 77) $fatal(1, "body second transaction");
    $display("pipeline module-body simulation passed");
    $finish;
end
endmodule
)";
    CHECK(out.good());
}

int main(int argc, char** argv) {
    testTradeoff(); testAlignment(); testApiAndErrors();
    if (argc == 2) emitBodyTestbench(argv[1]);
    std::cout << "pipeline tests passed\n";
}
