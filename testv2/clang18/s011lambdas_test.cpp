#include "s0clang18/s011lambdas.hpp"

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
    pred::s0clang18::FunctionReachabilityGraph reachability;
    pred::s0clang18::TemplateSpecializationTable templates;
};

static Fixture buildFixture() {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s011_virtual_input.logic.cpp";
    options.source_text = R"cpp(
#include <fixint.hpp>

#pragma input_port in_value
Int<8> in_value;
#pragma input_port sel
bool sel;
#pragma output_port out_value
Int<8> out_value;

void hls_main() {
    Int<8> base = in_value + Int<8>(1);
    Int<8> delta = in_value ^ Int<8>(3);

    auto by_copy = [=](Int<8> x) {
        if (sel) {
            return Int<8>(x + base);
        }
        return x ^ delta;
    };

    auto by_ref = [&](Int<8> x) {
        base = base + x;
        return base ^ delta;
    };

    auto generic = [=]<int K>(Int<8> x) {
        return Int<8>(by_copy(x) + Int<8>(K));
    };

    auto outer = [=](Int<8> x) {
        auto inner = [=](Int<8> y) {
            return Int<8>(y + base);
        };
        return inner(x);
    };

    out_value =
        by_copy(in_value) ^
        by_ref(delta) ^
        Int<8>(generic.template operator()<4>(base)) ^
        Int<8>(outer(delta));
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

    auto templates = pred::s0clang18::resolveTemplateSpecializations(
        fixture.session, fixture.reachability, fixture.const_eval, policy);
    CHECK(templates.ok());
    CHECK(templates.value.has_value());
    fixture.templates = std::move(*templates.value);
    return fixture;
}

static bool hasCapture(const pred::s0clang18::LambdaInfo& lambda,
                       const std::string& name,
                       pred::s0clang18::LambdaCaptureKind kind) {
    for (const auto& capture : lambda.captures) {
        if (capture.source_name == name && capture.kind == kind) return true;
    }
    return false;
}

static bool hasCaptureWithType(const pred::s0clang18::LambdaInfo& lambda,
                               const std::string& name,
                               pred::s0clang18::LambdaCaptureKind kind,
                               const std::string& hw_kind,
                               int width) {
    for (const auto& capture : lambda.captures) {
        if (capture.source_name != name || capture.kind != kind) continue;
        return capture.type.hw_kind == hw_kind && capture.type.width == width;
    }
    return false;
}

static int countLambdaKind(const pred::s0clang18::FunctionReachabilityGraph& graph,
                           pred::s0clang18::FunctionEntityKind kind) {
    int count = 0;
    for (const auto& function : graph.functions) {
        if (function.kind == kind) ++count;
    }
    return count;
}

int main() {
    Fixture fixture = buildFixture();
    pred::s0clang18::SourceLocPolicy policy;
    policy.canonicalize_paths = false;

    auto result = pred::s0clang18::resolveLambdaCaptures(
        fixture.session, fixture.reachability, fixture.type_context, policy);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());

    const auto& table = *result.value;
    CHECK(table.lambdas.size() >= 5);
    CHECK(countLambdaKind(fixture.reachability,
                          pred::s0clang18::FunctionEntityKind::Lambda) >= 4);
    CHECK(countLambdaKind(fixture.reachability,
                          pred::s0clang18::FunctionEntityKind::GenericLambdaSpecialization) >= 1);

    bool saw_by_copy = false;
    bool saw_by_ref = false;
    bool saw_generic = false;
    int base_only_copy_lambdas = 0;

    for (const auto& lambda : table.lambdas) {
        CHECK(lambda.function_id >= 0);
        CHECK(lambda.lambda_expr != nullptr);
        CHECK(lambda.call_operator != nullptr);
        CHECK(lambda.loc.valid());
        CHECK(pred::s0clang18::findLambdaInfo(table, lambda.function_id) == &lambda);

        const auto* function =
            pred::s0clang18::findFunctionEntity(fixture.reachability, lambda.function_id);
        CHECK(function != nullptr);

        if (function->kind == pred::s0clang18::FunctionEntityKind::GenericLambdaSpecialization) {
            saw_generic = hasCapture(lambda, "by_copy",
                                     pred::s0clang18::LambdaCaptureKind::ByCopy);
        }
        if (hasCaptureWithType(lambda, "base",
                               pred::s0clang18::LambdaCaptureKind::ByCopy,
                               "Int", 8) &&
            hasCapture(lambda, "delta", pred::s0clang18::LambdaCaptureKind::ByCopy)) {
            saw_by_copy = true;
        }
        if (hasCaptureWithType(lambda, "base",
                               pred::s0clang18::LambdaCaptureKind::ByReference,
                               "Int", 8) &&
            hasCapture(lambda, "delta", pred::s0clang18::LambdaCaptureKind::ByReference)) {
            saw_by_ref = true;
        }
        if (hasCapture(lambda, "base", pred::s0clang18::LambdaCaptureKind::ByCopy) &&
            lambda.captures.size() == 1) {
            ++base_only_copy_lambdas;
        }
    }

    if (!saw_by_copy || !saw_by_ref || !saw_generic || base_only_copy_lambdas < 2) {
        for (const auto& lambda : table.lambdas) {
            const auto* function =
                pred::s0clang18::findFunctionEntity(fixture.reachability, lambda.function_id);
            std::cerr << "lambda #" << lambda.function_id
                      << " kind=" << (function ? static_cast<int>(function->kind) : -1)
                      << " captures:";
            for (const auto& capture : lambda.captures) {
                std::cerr << " " << capture.source_name
                          << "/" << static_cast<int>(capture.kind)
                          << "/" << capture.type.hw_kind
                          << "/" << capture.type.width
                          << "/ref=" << capture.type.is_reference;
            }
            std::cerr << "\n";
        }
    }

    CHECK(saw_by_copy);
    CHECK(saw_by_ref);
    CHECK(saw_generic);
    CHECK(base_only_copy_lambdas >= 2);
    CHECK(pred::s0clang18::findLambdaInfo(table, -1) == nullptr);

    std::cout << "s011lambdas_test passed\n";
    return 0;
}
