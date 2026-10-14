#include "rtlzz.hpp"
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << "\n"; std::exit(1); } } while(false)

static rtlzz::CompileOptions options() {
    rtlzz::CompileOptions o;
    o.top_function="logic";
    o.vullib_dir=std::filesystem::exists("vullib/fixint.hpp") ? "vullib" : "third_party/vulsim/vullib";
    o.register_stages=3;
    o.reset_enabled=true;
    o.source_codelines={R"(
#include <array>
#include <cstdint>
#include <fixint.hpp>
#pragma input_port a
Int<80> a;
#pragma input_port index
uint8_t index;
#pragma input_port v
bool v;
#pragma input_port signed_input
Int<8> signed_input;
#pragma output_port y
std::array<Int<80>,2> y;
#pragma output_port lookup
uint8_t lookup;
#pragma output_port q
Int<8> q;
#pragma output_port ov
bool ov;
#pragma output_port patched
Int<80> patched;
void logic() {
    static const uint8_t table[4]={3,11,29,47};
    Int<80> w=a;
    w.at<79,64>()=a.at<15,0>();
    w=w^Int<80>(255);
    y[0]=w+Int<80>(3);
    y[1]=a<<Int<7>(3);
    lookup=table[index];
    q=signed_input.sint() >> 1u;
    ov=v;
    patched=a;
    patched.pick<4>(uint32_t(index)*4u)=Int<4>(9);
}
)"};
    o.input_reset_values={{"a",{0x0123456789abcde0ULL,0x4321}}, {"index",{1}}, {"v",{0}}, {"signed_input",{250}}};
    return o;
}

int main(int argc, char**argv) {
    for (int stages : {0,1,3,8}) {
        auto o=options(); o.register_stages=stages;
        auto r=rtlzz::compileToRtl(o);
        if (!r.ok()) std::cerr << r.error << "\n";
        CHECK(r.ok()); CHECK(r.pipeline_report);
        const auto &values=r.pipeline_report->output_reset_values;
        CHECK(values.at("y")==std::vector<std::uint64_t>({0x0123456789abcd22ULL,0xcde0,0x091a2b3c4d5e6f00ULL,0x1908}));
        CHECK(values.at("lookup")==std::vector<std::uint64_t>({11}));
        CHECK(values.at("ov")==std::vector<std::uint64_t>({0}));
        CHECK(values.at("patched")==std::vector<std::uint64_t>({0x0123456789abcd90ULL,0x4321}));
        CHECK(values.at("q")==std::vector<std::uint64_t>({253}));
        std::string rtl; for (auto &line:r.output_codelines) rtl+=line;
        CHECK((rtl.find("if (!rstn)")!=std::string::npos)==(stages!=0));
    }
    auto invalid=options(); invalid.input_reset_values.erase("v"); CHECK(!rtlzz::compileToRtl(invalid).ok());
    invalid=options(); invalid.input_reset_values["a"]={1}; CHECK(!rtlzz::compileToRtl(invalid).ok());
    invalid=options(); invalid.input_reset_values["a"][1]=0x10000; CHECK(!rtlzz::compileToRtl(invalid).ok());
    invalid=options(); invalid.input_reset_values["unknown"]={0}; CHECK(!rtlzz::compileToRtl(invalid).ok());
    invalid=options(); invalid.reset_name="clk"; CHECK(!rtlzz::compileToRtl(invalid).ok());
    invalid=options(); invalid.reset_name="a"; CHECK(!rtlzz::compileToRtl(invalid).ok());
    invalid=options(); invalid.reset_enabled=false; CHECK(!rtlzz::compileToRtl(invalid).ok());
    auto active_high=options(); active_high.reset_name="rst"; active_high.reset_active_low=false;
    auto high=rtlzz::compileToRtl(active_high); CHECK(high.ok());
    std::string text; for (auto &line:high.output_codelines) text+=line;
    CHECK(text.find("if (rst)")!=std::string::npos);
    if (argc==2) {
        auto o=options(); o.rtl_module_body=true;
        o.rtl_port_bindings={{"a","external_a"},{"index","external_index"},{"v","external_v"},{"signed_input","external_signed"},{"y","external_y"},{"lookup","external_lookup"},{"q","external_q"},{"ov","external_ov"},{"patched","external_patched"},{"clk","external_clk"},{"rstn","external_rstn"}};
        auto r=rtlzz::compileToRtl(o); CHECK(r.ok());
        std::ofstream file(argv[1]); CHECK(file.good());
        file << "module reset_tb; logic external_clk,external_rstn,external_v,external_ov; logic [79:0] external_a,external_y[0:1],external_patched; logic [7:0] external_index,external_signed,external_lookup,external_q;\n";
        for (auto &line:r.output_codelines) file<<line;
        file << R"(
initial begin
    external_clk=0; external_rstn=0; external_a=0; external_index=2; external_signed=123; external_v=1;
    repeat(4) begin #1; external_clk=1; #1; external_clk=0; end
    if (external_y[0]!==80'hcde00123456789abcd22 || external_y[1]!==80'h1908091a2b3c4d5e6f00 || external_lookup!==11 || external_q!==253 || external_ov!==0 || external_patched!==80'h43210123456789abcd90) $fatal(1,"reset hold");
    external_rstn=1;
    repeat(3) begin #1; external_clk=1; #1; external_clk=0; end
    if (external_y[0]!==258 || external_y[1]!==0 || external_lookup!==29 || external_q!==61 || external_ov!==1 || external_patched!==2304) $fatal(1,"reset release alignment");
    external_rstn=0; external_a=9;
    #1; external_clk=1; #1; external_clk=0;
    if (external_y[0]!==80'hcde00123456789abcd22 || external_lookup!==11 || external_ov!==0) $fatal(1,"mid-flight reset");
    $display("pipeline reset simulation passed"); $finish;
end
endmodule
)";
    }
    std::cout << "pipeline reset API tests passed\n";
}
