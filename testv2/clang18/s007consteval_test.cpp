#include "s0clang18/s007consteval.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
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

class DeclCollector : public clang::RecursiveASTVisitor<DeclCollector> {
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
    options.source_name = "testv2/clang18/s007_virtual_input.logic.cpp";
    options.source_text = R"cpp(
#include <array>
#include <cstdint>
#include <fixint.hpp>

constexpr int constexpr_helper(int x, bool high) {
    if (high) {
        return x * 2 + 3;
    }
    return x - 1;
}

template <int N>
struct Box {
    static constexpr int value = N;
};

void hls_main(int runtime_input) {
    constexpr int value = constexpr_helper(5, true);
    constexpr bool cond = constexpr_helper(2, false) == 1;
    int non_const = runtime_input + 1;
    Box<constexpr_helper(4, true)> box;
    (void)value;
    (void)cond;
    (void)non_const;
    (void)box;
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

bool failed(const std::vector<pred::s0clang18::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == pred::s0clang18::Severity::Error) return true;
    }
    return false;
}

const clang::TemplateArgument& firstTemplateArg(clang::VarDecl* var) {
    clang::QualType type = var->getType().getUnqualifiedType();
    const auto* record = type->getAs<clang::RecordType>();
    CHECK(record != nullptr);
    const auto* specialization =
        llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record->getDecl());
    CHECK(specialization != nullptr);
    CHECK(specialization->getTemplateArgs().size() >= 1);
    return specialization->getTemplateArgs()[0];
}

} // namespace

int main() {
    auto session = buildSession();
    DeclCollector collector;
    collector.TraverseDecl(session.ast_context->getTranslationUnitDecl());

    pred::s0clang18::TypeLoweringContext type_context;
    type_context.session = &session;
    pred::s0clang18::ConstEvalContext context;
    context.session = &session;
    context.type_context = &type_context;

    auto int_result = pred::s0clang18::evalIntegerExpr(
        context, collector.vars.at("value")->getInit());
    CHECK(int_result.ok());
    CHECK(int_result.value.has_value());
    CHECK(*int_result.value == 13);

    auto bool_result = pred::s0clang18::evalBoolExpr(
        context, collector.vars.at("cond")->getInit());
    CHECK(bool_result.ok());
    CHECK(bool_result.value.has_value());
    CHECK(*bool_result.value);

    auto const_value = pred::s0clang18::evalConstExpr(
        context, collector.vars.at("cond")->getInit());
    CHECK(const_value.ok());
    CHECK(const_value.value.has_value());
    CHECK(const_value.value->kind == pred::s0clang18::ConstValue::Kind::Bool);
    CHECK(const_value.value->boolean);

    auto template_arg = pred::s0clang18::evalTemplateIntegralArgument(
        context, firstTemplateArg(collector.vars.at("box")));
    CHECK(template_arg.ok());
    CHECK(template_arg.value.has_value());
    CHECK(*template_arg.value == 11);

    auto non_const = pred::s0clang18::evalIntegerExpr(
        context, collector.vars.at("non_const")->getInit());
    CHECK(!non_const.ok());
    CHECK(!non_const.value.has_value());
    CHECK(failed(non_const.diagnostics));

    auto wrong_kind = pred::s0clang18::evalTemplateIntegralArgument(
        context, clang::TemplateArgument());
    CHECK(!wrong_kind.ok());
    CHECK(failed(wrong_kind.diagnostics));

    std::cout << "s007consteval_test passed\n";
    return 0;
}

