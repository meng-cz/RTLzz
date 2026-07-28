#include "s0clang18/s009reachability.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

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
};

static Fixture buildFixture() {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s009_virtual_input.logic.cpp";
    options.source_text = R"cpp(
#include <fixint.hpp>

Int<8> plain(Int<8> x) {
    return x;
}

template <int N>
Int<8> templated(Int<8> x) {
    return plain(x);
}

Int<8> unused(Int<8> x) {
    return plain(x);
}

struct Worker {
    Int<8> bump(Int<8> x) {
        return templated<2>(x);
    }
};

#pragma input_port input_value
Int<8> input_value;
#pragma output_port output_value
Int<8> output_value;

void hls_main() {
    Worker worker{};
    auto lambda = [](Int<8> x) {
        return plain(x);
    };
    auto generic = []<int N>(Int<8> x) {
        return templated<N>(x);
    };
    auto never_called = [](Int<8> x) {
        return unused(x);
    };
    (void)never_called;
    output_value = generic.template operator()<3>(worker.bump(lambda(input_value)));
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
    return fixture;
}

static bool hasKind(const pred::s0clang18::FunctionReachabilityGraph& graph,
                    pred::s0clang18::FunctionEntityKind kind) {
    for (const auto& function : graph.functions) {
        if (function.kind == kind) return true;
    }
    return false;
}

static int countKind(const pred::s0clang18::FunctionReachabilityGraph& graph,
                     pred::s0clang18::FunctionEntityKind kind) {
    int count = 0;
    for (const auto& function : graph.functions) {
        if (function.kind == kind) ++count;
    }
    return count;
}

static bool hasStableNameFragment(const pred::s0clang18::FunctionReachabilityGraph& graph,
                                  const std::string& fragment) {
    for (const auto& function : graph.functions) {
        if (function.key.stable_name.find(fragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

static bool hasTemplateValue(const pred::s0clang18::FunctionReachabilityGraph& graph,
                             long long value) {
    for (const auto& function : graph.functions) {
        for (long long template_value : function.key.template_values) {
            if (template_value == value) return true;
        }
    }
    return false;
}

int main() {
    Fixture fixture = buildFixture();
    pred::s0clang18::SourceLocPolicy policy;
    policy.canonicalize_paths = false;

    auto result = pred::s0clang18::collectFunctionReachability(
        fixture.session, fixture.top, fixture.semantic, fixture.const_eval, policy);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());

    const auto& graph = *result.value;
    CHECK(graph.top_function == 0);
    CHECK(pred::s0clang18::findFunctionEntity(graph, graph.top_function) != nullptr);
    CHECK(graph.functions.size() >= 6);
    CHECK(graph.call_edges.size() >= 5);

    CHECK(countKind(graph, pred::s0clang18::FunctionEntityKind::Top) == 1);
    CHECK(hasKind(graph, pred::s0clang18::FunctionEntityKind::Helper));
    CHECK(hasKind(graph, pred::s0clang18::FunctionEntityKind::Method));
    CHECK(hasKind(graph, pred::s0clang18::FunctionEntityKind::Lambda));
    CHECK(hasKind(graph, pred::s0clang18::FunctionEntityKind::GenericLambdaSpecialization));
    CHECK(hasKind(graph, pred::s0clang18::FunctionEntityKind::FunctionTemplateSpecialization));

    CHECK(hasStableNameFragment(graph, "plain"));
    CHECK(hasStableNameFragment(graph, "templated"));
    CHECK(hasTemplateValue(graph, 2));
    CHECK(hasTemplateValue(graph, 3));
    CHECK(!hasStableNameFragment(graph, "unused"));

    for (const auto& function : graph.functions) {
        CHECK(function.id >= 0);
        CHECK(function.function_decl != nullptr);
        CHECK(!function.key.stable_name.empty());
        CHECK(graph.function_by_stable_name.count(function.key.stable_name) == 1);
    }

    for (const auto& edge : graph.call_edges) {
        CHECK(pred::s0clang18::findFunctionEntity(graph, edge.caller) != nullptr);
        CHECK(pred::s0clang18::findFunctionEntity(graph, edge.callee) != nullptr);
        CHECK(edge.call_expr != nullptr);
        CHECK(edge.loc.valid());
    }

    std::cout << "s009reachability_test passed\n";
    return 0;
}
