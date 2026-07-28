#include "s0clang18/s005semantic.hpp"

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
};

static Fixture buildFixture() {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s005_virtual_input.logic.cpp";
    options.source_text = R"cpp(
#include <fixint.hpp>

struct Packet {
    Int<8> payload;
    bool valid;
    Int<8> method(Int<8> x) { return x; }
};

template <int N>
Int<N> templated(Int<N> x) {
    return x;
}

Int<8> helper(Int<8> x) {
    return templated<8>(x);
}

#pragma input_port input_value
Int<8> input_value;
#pragma output_port output_value
Int<8> output_value;

void hls_main() {
    Int<8> local = helper(input_value);
    auto lambda = [](Int<8> x) { return x; };
    output_value = lambda(local);
}
)cpp";
    options.top_function = "hls_main";
    options.clang_args = {"-I.", "-Ithird_party/vulsim/vullib"};

    auto session_result = pred::s0clang18::createClang18Session(options);
    if (!session_result.ok()) {
        for (const auto& diagnostic : session_result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(session_result.ok());
    CHECK(session_result.value.has_value());

    pred::s0clang18::SourceLocPolicy policy;
    policy.canonicalize_paths = false;
    auto port_result = pred::s0clang18::collectPragmaAndPortDecls(
        *session_result.value, policy);
    CHECK(port_result.ok());
    CHECK(port_result.value.has_value());

    auto top_result = pred::s0clang18::selectTopFunction(
        *session_result.value, "hls_main", policy);
    CHECK(top_result.ok());
    CHECK(top_result.value.has_value());

    Fixture fixture;
    fixture.session = std::move(*session_result.value);
    fixture.ports = std::move(*port_result.value);
    fixture.top = std::move(*top_result.value);
    return fixture;
}

static bool hasKind(const pred::s0clang18::SemanticIndex& index,
                    const std::string& name,
                    pred::s0clang18::SemanticEntityKind kind) {
    auto found = index.entities_by_name.find(name);
    if (found == index.entities_by_name.end()) return false;
    for (pred::s0clang18::SemanticEntityId id : found->second) {
        const auto& entity = index.entities.at(static_cast<std::size_t>(id));
        if (entity.kind == kind) return true;
    }
    return false;
}

static int countKind(const pred::s0clang18::SemanticIndex& index,
                     pred::s0clang18::SemanticEntityKind kind) {
    int count = 0;
    for (const auto& entity : index.entities) {
        if (entity.kind == kind) ++count;
    }
    return count;
}

int main() {
    Fixture fixture = buildFixture();
    pred::s0clang18::SourceLocPolicy policy;
    policy.canonicalize_paths = false;
    auto result = pred::s0clang18::buildSemanticIndex(
        fixture.session, fixture.top, fixture.ports, policy);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());

    const auto& index = *result.value;
    CHECK(!index.entities.empty());
    CHECK(pred::s0clang18::findEntity(index, fixture.top.function_decl) != nullptr);
    CHECK(pred::s0clang18::findEntity(index, fixture.ports.ports[0].var_decl) != nullptr);
    CHECK(pred::s0clang18::findEntity(index, fixture.ports.ports[1].var_decl) != nullptr);

    CHECK(hasKind(index, "hls_main", pred::s0clang18::SemanticEntityKind::Function));
    CHECK(hasKind(index, "helper", pred::s0clang18::SemanticEntityKind::Function));
    CHECK(hasKind(index, "templated", pred::s0clang18::SemanticEntityKind::FunctionTemplate));
    CHECK(hasKind(index, "Packet", pred::s0clang18::SemanticEntityKind::Record));
    CHECK(hasKind(index, "payload", pred::s0clang18::SemanticEntityKind::Field));
    CHECK(hasKind(index, "method", pred::s0clang18::SemanticEntityKind::Method));
    CHECK(hasKind(index, "input_value", pred::s0clang18::SemanticEntityKind::Variable));
    CHECK(hasKind(index, "output_value", pred::s0clang18::SemanticEntityKind::Variable));
    CHECK(hasKind(index, "local", pred::s0clang18::SemanticEntityKind::Variable));
    CHECK(countKind(index, pred::s0clang18::SemanticEntityKind::LambdaCallOperator) >= 1);

    for (const auto& entity : index.entities) {
        CHECK(entity.id >= 0);
        CHECK(entity.decl != nullptr);
        CHECK(entity.loc.valid());
    }

    std::cout << "s005semantic_test passed\n";
    return 0;
}

