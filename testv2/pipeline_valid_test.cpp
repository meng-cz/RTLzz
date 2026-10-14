#include "backend/pipeline.hpp"
#include "backend/rtlgen.hpp"
#include "rtlzz.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; std::exit(1); } } while(false)
using namespace pred;
using namespace beir;

static Operand ref(const Program& p, NodeId id) {
    Operand v; v.node=id; v.type=p.signal(id).type; return v;
}
static NodeId node(Program& p, std::string name, int width, OperationKind kind, std::vector<Operand> args, OpCode code=OpCode::None) {
    Signal s; s.id=p.signals.size(); s.name=name; s.type.width=width;
    Operation op; op.kind=kind; op.type=s.type; op.operands=std::move(args); op.op=code;
    s.driver=op; p.signals.push_back(s); return s.id;
}
static NodeId input(Program& p, std::string name, int width) {
    Operand v; v.kind=OperandKind::Port; v.text=name; v.type.width=width;
    auto id=node(p,name,width,OperationKind::PortRead,{v});
    Port port; port.name=name; port.direction=PortDirection::Input; port.type.width=width; port.element_nodes={id};
    p.ports.push_back(port); p.inputs.push_back(name); return id;
}
static void output(Program& p, std::string name, NodeId value) {
    auto id=node(p,name,p.signal(value).type.width,OperationKind::Assign,{ref(p,value)});
    Port port; port.name=name; port.direction=PortDirection::Output; port.type=p.signal(id).type; port.element_nodes={id};
    p.ports.push_back(port); p.outputs.push_back(name);
}
static void structure() {
    Program p; p.function_name="valid_graph";
    auto a=input(p,"a",32), v=input(p,"v",1);
    auto x=node(p,"x",32,OperationKind::Binary,{ref(p,a),ref(p,a)},OpCode::Mul);
    auto first=node(p,"first",32,OperationKind::Ite,{ref(p,v),ref(p,x),ref(p,a)});
    auto y=node(p,"y",32,OperationKind::Binary,{ref(p,first),ref(p,x)},OpCode::Mul);
    auto last=node(p,"last",32,OperationKind::Ite,{ref(p,v),ref(p,y),ref(p,a)});
    output(p,"out",last); output(p,"short_path",first); output(p,"ov",v);
    for (int n : {0,1,2,4,8}) {
        pipeline::Options o; o.register_stages=n; o.input_valid_name="v"; o.output_valid_name="ov";
        auto r=pipeline::build(p,o); pipeline::validateProgram(r);
        CHECK(r.valid_nodes.size()==static_cast<std::size_t>(n+1));
        std::size_t count=0; std::uint64_t bits=0;
        for (const auto& reg:r.registers) {
            if (reg.is_valid) { ++count; CHECK(reg.enable==kInvalidNodeId); }
            else CHECK(reg.enable==r.valid_nodes.at(reg.stage-1));
            bits+=r.rtl_program.signal(reg.node).type.width;
        }
        CHECK(count==static_cast<std::size_t>(n)); CHECK(bits==r.report.register_bits);
        for (auto id : {first,last})
            CHECK(r.rtl_program.signal(id).driver->operands[0].node==r.valid_nodes.at(r.stages.at(id)));
        if (n) {
            auto bad=r;
            auto data=std::find_if(bad.registers.begin(),bad.registers.end(),[](const auto& reg) {return !reg.is_valid;});
            CHECK(data!=bad.registers.end()); data->enable=kInvalidNodeId;
            bool rejected=false; try {pipeline::validateProgram(bad);} catch (...) {rejected=true;} CHECK(rejected);
        }
    }
}
static rtlzz::CompileOptions options(int n) {
    rtlzz::CompileOptions o; o.top_function="logic";
    o.vullib_dir=std::filesystem::exists("vullib/fixint.hpp") ? "vullib" : "third_party/vulsim/vullib";
    o.register_stages=n; o.input_valid_name="iv"; o.output_valid_name="ov";
    o.reset_enabled=true; o.input_reset_values={{"a",{0}},{"iv",{0}}};
    o.source_codelines={R"(
#include <array>
#include <cstdint>
#pragma input_port a
uint32_t a;
#pragma input_port iv
bool iv;
#pragma output_port ov
bool ov;
#pragma output_port y
std::array<uint32_t,2> y;
uint32_t helper(uint32_t x) { return iv ? x+5u : x+9u; }
void logic() {
    if (a==0xffffffffu) { y[0]=123; y[1]=456; return; }
    uint32_t x=(a+1u)*(a+3u);
    y[0]=(helper(x)*7u)^a;
    y[1]=iv ? a+1u : a+2u;
}
)"};
    return o;
}
static std::string text(const rtlzz::CompileResult& r) {
    std::string s; for (const auto& line:r.output_codelines) s+=line; return s;
}
int main(int argc,char**argv) {
    structure();
    std::ofstream file;
    if (argc==2) {file.open(argv[1]); CHECK(file.good()); file<<"`timescale 1ns/1ps\n";}
    for (int n : {0,1,2,4,8}) {
        auto o=options(n); auto r=rtlzz::compileToRtl(o);
        if (!r.ok()) std::cerr<<r.error<<"\n";
        CHECK(r.ok()); CHECK(r.pipeline_report);
        CHECK(r.pipeline_report->output_reset_values.at("ov")==std::vector<std::uint64_t>{0});
        CHECK(r.pipeline_report->output_reset_values.at("y")==std::vector<std::uint64_t>({84,2}));
        if (!n) CHECK(text(r).find("always_ff")==std::string::npos);
        else CHECK(text(r).find("if (iv)")!=std::string::npos);
        // Port metadata keeps the backend-owned output without requiring source writes.
        auto meta=rtlzz::compileToPortMetadata(o); CHECK(meta.ok());
        CHECK(text(meta).find("\"name\": \"ov\"")!=std::string::npos);
        CHECK(text(meta).find("\"input_valid_name\": \"iv\"")!=std::string::npos);
        if (!n) CHECK(text(meta).find("clock_name")==std::string::npos);
        o.rtl_module_body=true;
        o.rtl_port_bindings={{"a","input_q"},{"iv","valid_q"},{"ov","valid_d"},{"y","data_d"},{"clk","clk"},{"rstn","rstn"}};
        auto body=rtlzz::compileToRtl(o); CHECK(body.ok());
        if (file.is_open()) {
            file<<"module valid_case_"<<n<<"(input logic clk,rstn,input logic [31:0] sample,input logic sample_valid,output logic [31:0] data0,data1,output logic valid);\n";
            file<<"logic [31:0] input_q,data_d[0:1]; logic valid_q,valid_d;\n";
            file<<"always_ff @(posedge clk) begin if (!rstn) begin input_q<=0;valid_q<=0;data0<=84;data1<=2;valid<=0;end else begin input_q<=sample;valid_q<=sample_valid;data0<=data_d[0];data1<=data_d[1];valid<=valid_d;end end\n";
            const auto rtl=text(body);
            file<<rtl;
            // Observe actual FF storage around each clock edge, including bubbles.
            const std::regex writes(R"(if \(([A-Za-z_][A-Za-z_0-9]*)\) begin\s+([A-Za-z_][A-Za-z_0-9]*(?:\[[0-9]+\])?) <=)");
            for (auto it=std::sregex_iterator(rtl.begin(),rtl.end(),writes); it!=std::sregex_iterator(); ++it) {
                const auto enable=(*it)[1].str(), q=(*it)[2].str();
                file<<"always @(posedge clk) begin logic [$bits("<<q<<")-1:0] saved;logic en;saved="<<q<<";en="<<enable<<";#0.1;if(rstn && !en && "<<q<<"!==saved) $fatal(1,\"invalid data write\");end\n";
            }
            file<<"endmodule\n";
        }
    }
    auto bad=options(2); bad.output_valid_name.clear(); CHECK(!rtlzz::compileToRtl(bad).ok());
    bad=options(2); bad.input_valid_name="missing"; CHECK(!rtlzz::compileToRtl(bad).ok());
    bad=options(2); bad.input_valid_name="a"; CHECK(!rtlzz::compileToRtl(bad).ok());
    bad=options(2); std::swap(bad.input_valid_name,bad.output_valid_name); CHECK(!rtlzz::compileToRtl(bad).ok());
    bad=options(2); bad.output_valid_name="iv"; CHECK(!rtlzz::compileToRtl(bad).ok());
    bad=options(2); bad.source_codelines[0]+="\n"; auto pos=bad.source_codelines[0].find("uint32_t x="); bad.source_codelines[0].insert(pos,"ov=iv; "); CHECK(!rtlzz::compileToRtl(bad).ok());
    bad=options(2); bad.source_codelines[0].replace(bad.source_codelines[0].find("bool ov;"),8,"std::array<bool,2> ov;"); CHECK(!rtlzz::compileToRtl(bad).ok());
    for (int n : {0,2}) { auto plain=options(n); plain.reset_enabled=false; plain.input_reset_values.clear(); CHECK(rtlzz::compileToRtl(plain).ok()); }
    bad=options(0); bad.use_circt=true; CHECK(!rtlzz::compileToRtl(bad).ok());
    if (file.is_open()) {
        file<<"module valid_tb; logic clk=0,rstn=0;logic [31:0] sample;logic sample_valid;logic [31:0] d0[0:4],d1[0:4];logic v[0:4];\n";
        int index=0; for (int n:{0,1,2,4,8}) file<<"valid_case_"<<n<<" c"<<index<<"(clk,rstn,sample,sample_valid,d0["<<index<<"],d1["<<index<<"],v["<<index++<<"]);\n";
        file<<R"(
logic [31:0] samples[0:79];logic flags[0:79];
function automatic logic [31:0] expected0(input logic [31:0] a);
if (a==32'hffffffff) return 123;
return ((((a+32'd1)*(a+32'd3))+32'd5)*32'd7)^a;
endfunction
initial begin
sample=0;sample_valid=0;
for (int pass=0;pass<2;pass++) begin
rstn=0;
repeat(2) begin #1;clk=1;#1;clk=0;end
for (int j=0;j<5;j++) if (v[j]!==0) $fatal(1,"reset valid");
rstn=1;
for (int i=0;i<80;i++) begin
sample=(i==10) ? 32'hffffffff : (i*32'd91837+32'd71);
sample_valid=(i%5==0 || (i>=18 && i<26) || (i>=50 && i<60));
samples[i]=sample;flags[i]=sample_valid;
#1;clk=1;#1;clk=0;
)";
        index=0; for (int n:{0,1,2,4,8}) {
            file<<"if (i<"<<n+1<<") begin if(v["<<index<<"]!==0) $fatal(1,\"fill valid\");end else begin\n";
            file<<"if(v["<<index<<"]!==flags[i-"<<n+1<<"]) $fatal(1,\"valid alignment stages="<<n<<" cycle=%d\",i);\n";
            file<<"if(flags[i-"<<n+1<<"]) begin if(d0["<<index<<"]!==expected0(samples[i-"<<n+1<<"]) || d1["<<index<<"]!==((samples[i-"<<n+1<<"]==32'hffffffff)?32'd456:samples[i-"<<n+1<<"]+32'd1)) $fatal(1,\"data alignment stages="<<n<<" cycle=%d\",i);end end\n";
            ++index;
        }
        file<<"end end $display(\"pipeline valid simulation passed\");$finish;end endmodule\n";
    }
    std::cout<<"pipeline valid API and IR tests passed\n";
}
