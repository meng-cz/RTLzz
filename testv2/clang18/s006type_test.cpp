#include "s0clang18/s006type.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>

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

namespace {

class VarCollector : public clang::RecursiveASTVisitor<VarCollector> {
public:
    bool VisitVarDecl(clang::VarDecl* decl) {
        if (!decl->getIdentifier()) return true;
        vars[decl->getNameAsString()] = decl;
        return true;
    }

    std::unordered_map<std::string, clang::VarDecl*> vars;
};

pred::s0clang18::Clang18Session buildSession() {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s006_virtual_input.logic.cpp";
    options.source_text = R"cpp(
#include <array>
#include <cstdint>
#include <fixint.hpp>

struct Packet {
    Int<8> payload;
    bool valid;
};

enum class Mode : int8_t {
    Neg = -1,
    Pos = 7,
};

void hls_main() {
    bool flag = false;
    uint8_t byte_value = 0;
    int signed_value = 0;
    Mode mode_value = Mode::Pos;
    Int<12> fixed_value;
    std::array<Int<8>, 2> array_value;
    std::array<std::array<uint16_t, 2>, 3> nested_array_value;
    Packet packet_value;
    const Packet& packet_ref = packet_value;
    Packet* packet_ptr = &packet_value;
    __int128 int128_value = 0;
}
)cpp";
    options.top_function = "hls_main";
    options.clang_args = {"-I.", "-Ithird_party/vulsim/vullib"};

    auto result = pred::s0clang18::createClang18Session(options);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());
    return std::move(*result.value);
}

pred::s0clang18::LoweredType lower(
    const pred::s0clang18::TypeLoweringContext& context,
    clang::QualType type) {
    auto result = pred::s0clang18::lowerQualType(context, type);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());
    return *result.value;
}

bool lowerFails(const pred::s0clang18::TypeLoweringContext& context,
                clang::QualType type) {
    auto result = pred::s0clang18::lowerQualType(context, type);
    return !result.ok() && !result.value.has_value();
}

} // namespace

int main() {
    auto session = buildSession();
    VarCollector vars;
    vars.TraverseDecl(session.ast_context->getTranslationUnitDecl());

    pred::s0clang18::TypeLoweringContext context;
    context.session = &session;

    auto flag = lower(context, vars.vars.at("flag")->getType());
    CHECK(flag.type.name == "bool");
    CHECK(flag.type.width == 1);
    CHECK(flag.type.hw_kind == "bool");

    auto byte_value = lower(context, vars.vars.at("byte_value")->getType());
    CHECK(byte_value.type.width == 8);
    CHECK(byte_value.type.hw_kind == "builtin");
    CHECK(!byte_value.type.is_signed);

    auto signed_value = lower(context, vars.vars.at("signed_value")->getType());
    CHECK(signed_value.type.width == 32);
    CHECK(signed_value.type.is_signed);

    auto mode_value = lower(context, vars.vars.at("mode_value")->getType());
    CHECK(mode_value.type.width == 8);
    CHECK(mode_value.type.is_signed);
    CHECK(mode_value.type.hw_kind == "enum");

    auto fixed_value = lower(context, vars.vars.at("fixed_value")->getType());
    CHECK(fixed_value.type.hw_kind == "Int");
    CHECK(fixed_value.type.width == 12);

    auto array_value = lower(context, vars.vars.at("array_value")->getType());
    CHECK(array_value.type.is_array);
    CHECK(array_value.type.array_size == 2);
    CHECK(array_value.type.array_dims.size() == 1);
    CHECK(array_value.type.array_dims[0] == 2);
    CHECK(array_value.type.hw_kind == "Int");
    CHECK(array_value.type.width == 8);

    auto nested = lower(context, vars.vars.at("nested_array_value")->getType());
    CHECK(nested.type.is_array);
    CHECK(nested.type.array_dims.size() == 2);
    CHECK(nested.type.array_dims[0] == 3);
    CHECK(nested.type.array_dims[1] == 2);
    CHECK(nested.type.width == 16);

    auto packet = lower(context, vars.vars.at("packet_value")->getType());
    CHECK(packet.type.struct_name == "Packet");
    CHECK(packet.record_key.has_value());
    CHECK(packet.record_key->canonical_name == "Packet");

    auto packet_ref = lower(context, vars.vars.at("packet_ref")->getType());
    CHECK(packet_ref.type.struct_name == "Packet");
    CHECK(packet_ref.type.is_reference);
    CHECK(packet_ref.type.is_const);
    CHECK(packet_ref.record_key.has_value());
    CHECK(pred::s0clang18::typeLabel(packet_ref.type) == "const Packet&");

    auto record_key = pred::s0clang18::canonicalRecordKey(
        context, vars.vars.at("packet_ref")->getType());
    CHECK(record_key.ok());
    CHECK(record_key.value.has_value());
    CHECK(record_key.value->canonical_name == "Packet");

    CHECK(lowerFails(context, vars.vars.at("packet_ptr")->getType()));
    CHECK(lowerFails(context, vars.vars.at("int128_value")->getType()));

    pred::s0clang18::TypeLoweringContext pointer_context = context;
    pointer_context.options.allow_pointer_types = true;
    auto ptr = lower(pointer_context, vars.vars.at("packet_ptr")->getType());
    CHECK(ptr.type.is_pointer);
    CHECK(ptr.type.struct_name == "Packet");

    std::cout << "s006type_test passed\n";
    return 0;
}
