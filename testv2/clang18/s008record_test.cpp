#include "s0clang18/s008record.hpp"

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
};

static Fixture buildFixture(const std::string& source) {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s008_virtual_input.logic.cpp";
    options.source_text = source;
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
    return fixture;
}

static bool hasError(const std::vector<pred::s0clang18::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == pred::s0clang18::Severity::Error) return true;
    }
    return false;
}

static void testCollectsRecordFieldsAndConstructors() {
    Fixture fixture = buildFixture(R"cpp(
#include <fixint.hpp>

struct Payload {
    Int<8> data;
    bool valid;
};

struct WithCtor {
    Int<8> data;
    bool valid;
    WithCtor(Int<8> d, bool v) : data(d), valid(v) {}
};

#pragma input_port input_value
Int<8> input_value;
#pragma output_port output_value
Int<8> output_value;

void hls_main() {
    Payload payload{input_value, true};
    WithCtor constructed(input_value, false);
    output_value = payload.data + constructed.data;
}
)cpp");

    pred::s0clang18::SourceLocPolicy policy;
    policy.canonicalize_paths = false;
    auto result = pred::s0clang18::collectRecordMetadata(
        fixture.session, fixture.top, fixture.ports, fixture.type_context, policy);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());

    const auto& set = *result.value;
    CHECK(set.records.size() >= 2);

    auto payload_it = set.record_by_name.find("Payload");
    CHECK(payload_it != set.record_by_name.end());
    const auto& payload = set.records[payload_it->second];
    CHECK(payload.aggregate_initializable);
    CHECK(payload.fields.size() == 2);
    CHECK(payload.fields[0].name == "data");
    CHECK(payload.fields[0].type.hw_kind == "Int");
    CHECK(payload.fields[0].type.width == 8);
    CHECK(payload.fields[1].name == "valid");
    CHECK(payload.fields[1].type.hw_kind == "bool");

    auto ctor_it = set.record_by_name.find("WithCtor");
    CHECK(ctor_it != set.record_by_name.end());
    const auto& with_ctor = set.records[ctor_it->second];
    CHECK(!with_ctor.aggregate_initializable);
    CHECK(with_ctor.fields.size() == 2);
    CHECK(!with_ctor.constructors.empty());
    CHECK(with_ctor.constructors[0].param_names.size() == 2);
    CHECK(with_ctor.constructors[0].field_to_param.at("data") == "d");
    CHECK(with_ctor.constructors[0].field_to_param.at("valid") == "v");

    const auto* found = pred::s0clang18::findRecordMetadata(set, with_ctor.key);
    CHECK(found == &with_ctor);
}

static void testRejectsReferenceField() {
    Fixture fixture = buildFixture(R"cpp(
#include <fixint.hpp>

struct Bad {
    Int<8>& ref;
};

#pragma input_port input_value
Int<8> input_value;
#pragma output_port output_value
Int<8> output_value;

void hls_main() {
    output_value = input_value;
}
)cpp");

    pred::s0clang18::SourceLocPolicy policy;
    auto result = pred::s0clang18::collectRecordMetadata(
        fixture.session, fixture.top, fixture.ports, fixture.type_context, policy);
    CHECK(!result.ok());
    CHECK(!result.value.has_value());
    CHECK(hasError(result.diagnostics));
}

int main() {
    testCollectsRecordFieldsAndConstructors();
    testRejectsReferenceField();
    std::cout << "s008record_test passed\n";
    return 0;
}
