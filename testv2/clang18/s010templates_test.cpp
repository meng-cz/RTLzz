#include "s0clang18/s010templates.hpp"

#include <clang/AST/DeclTemplate.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

[[noreturn]] static void failCheck(const char* expr, const char* file, int line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expr << "\n";
    std::exit(1);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) failCheck(#condition, __FILE__, __LINE__); \
    } while (false)

struct Fixture {
    pred::s0clang18::Clang18Session session;
    pred::s0clang18::PortDeclTable ports;
    pred::s0clang18::TopFunctionSelection top;
    pred::s0clang18::SemanticIndex semantic;
    pred::s0clang18::TypeLoweringContext type_context;
    pred::s0clang18::ConstEvalContext const_eval;
    pred::s0clang18::FunctionReachabilityGraph reachability;
};

static Fixture buildFixture() {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s010_virtual_input.logic.cpp";
    options.source_text = R"cpp(
#include <fixint.hpp>

template <int N, bool Flag>
Int<8> templated(Int<8> x) {
    return x;
}

struct Worker {
    template <int K>
    Int<8> bump(Int<8> x) {
        return templated<K + 1, true>(x);
    }
};

#pragma input_port input_value
Int<8> input_value;
#pragma output_port output_value
Int<8> output_value;

void hls_main() {
    Worker worker{};
    auto generic = []<int L>(Int<8> x) {
        return templated<L * 2, false>(x);
    };
    output_value = generic.template operator()<4>(
        worker.template bump<3>(input_value));
}
)cpp";
    options.top_function = "hls_main";
    options.clang_args = {"-I.", "-Ithird_party/vulsim/vullib"};

    auto session = pred::s0clang18::createClang18Session(options);
    if (!session.ok()) {
        for (const auto& diagnostic : session.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(session.ok());
    CHECK(session.value.has_value());

    pred::s0clang18::SourceLocPolicy policy;
    policy.canonicalize_paths = false;
    auto ports = pred::s0clang18::collectPragmaAndPortDecls(*session.value, policy);
    CHECK(ports.ok());
    CHECK(ports.value.has_value());

    auto top = pred::s0clang18::selectTopFunction(*session.value, "hls_main", policy);
    CHECK(top.ok());
    CHECK(top.value.has_value());

    auto semantic = pred::s0clang18::buildSemanticIndex(
        *session.value, *top.value, *ports.value, policy);
    CHECK(semantic.ok());
    CHECK(semantic.value.has_value());

    Fixture fixture;
    fixture.session = std::move(*session.value);
    fixture.ports = std::move(*ports.value);
    fixture.top = std::move(*top.value);
    fixture.semantic = std::move(*semantic.value);
    fixture.type_context.session = &fixture.session;
    fixture.type_context.semantic_index = &fixture.semantic;
    fixture.const_eval.session = &fixture.session;
    fixture.const_eval.type_context = &fixture.type_context;

    auto reachability = pred::s0clang18::collectFunctionReachability(
        fixture.session, fixture.top, fixture.semantic, fixture.const_eval, policy);
    if (!reachability.ok()) {
        for (const auto& diagnostic : reachability.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(reachability.ok());
    CHECK(reachability.value.has_value());
    fixture.reachability = std::move(*reachability.value);
    return fixture;
}

static std::unordered_map<std::string, long long> bindingsByName(
    const pred::s0clang18::TemplateSpecializationInfo& info) {
    std::unordered_map<std::string, long long> bindings;
    for (const auto& binding : info.value_bindings) {
        bindings[binding.parameter_name] = binding.value;
    }
    return bindings;
}

static bool primaryNameContains(
    const pred::s0clang18::TemplateSpecializationInfo& info,
    const std::string& fragment) {
    if (!info.primary_template_decl) return false;
    std::string name = info.primary_template_decl->getNameAsString();
    return name.find(fragment) != std::string::npos;
}

static bool hasSpecializationBinding(
    const pred::s0clang18::TemplateSpecializationTable& table,
    const std::string& primary_name_fragment,
    const std::string& parameter_name,
    long long expected_value) {
    for (const auto& info : table.specializations) {
        if (!primaryNameContains(info, primary_name_fragment)) continue;
        auto bindings = bindingsByName(info);
        auto found = bindings.find(parameter_name);
        if (found != bindings.end() && found->second == expected_value) {
            return true;
        }
    }
    return false;
}

int main() {
    Fixture fixture = buildFixture();
    pred::s0clang18::SourceLocPolicy policy;
    policy.canonicalize_paths = false;

    auto result = pred::s0clang18::resolveTemplateSpecializations(
        fixture.session, fixture.reachability, fixture.const_eval, policy);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());

    const auto& table = *result.value;
    CHECK(table.specializations.size() >= 4);
    CHECK(hasSpecializationBinding(table, "templated", "N", 4));
    CHECK(hasSpecializationBinding(table, "templated", "Flag", 1));
    CHECK(hasSpecializationBinding(table, "templated", "N", 8));
    CHECK(hasSpecializationBinding(table, "templated", "Flag", 0));
    CHECK(hasSpecializationBinding(table, "bump", "K", 3));
    CHECK(hasSpecializationBinding(table, "operator", "L", 4));

    for (const auto& info : table.specializations) {
        CHECK(info.function_id >= 0);
        CHECK(info.specialization_decl != nullptr);
        CHECK(info.primary_template_decl != nullptr);
        CHECK(info.call_loc.valid());
        CHECK(pred::s0clang18::findTemplateSpecialization(
                  table, info.function_id) == &info);
        CHECK(!info.value_bindings.empty());
        for (const auto& binding : info.value_bindings) {
            CHECK(!binding.parameter_name.empty());
            CHECK(binding.loc.valid());
        }
    }

    CHECK(pred::s0clang18::findTemplateSpecialization(table, -1) == nullptr);

    std::cout << "s010templates_test passed\n";
    return 0;
}
