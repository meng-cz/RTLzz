#include "s0clang18/s014init.hpp"

#include <clang/AST/RecursiveASTVisitor.h>

#include <cstdlib>
#include <iostream>
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
    pred::s0clang18::RecordMetadataSet records;
    pred::s0clang18::FunctionReachabilityGraph reachability;
    pred::s0clang18::TemplateSpecializationTable templates;
    pred::s0clang18::LambdaCaptureTable lambdas;
};

class VarCollector : public clang::RecursiveASTVisitor<VarCollector> {
public:
    bool VisitVarDecl(clang::VarDecl* decl) {
        if (!decl || !decl->getIdentifier()) return true;
        vars[decl->getNameAsString()] = decl;
        return true;
    }

    std::unordered_map<std::string, clang::VarDecl*> vars;
};

static Fixture buildFixture() {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s014_virtual_input.logic.cpp";
    options.source_text = R"cpp(
#include <array>
#include <fixint.hpp>

struct Pair {
    Int<8> lo;
    Int<8> hi;
};

#pragma input_port a
Int<8> a;
#pragma input_port b
Int<8> b;
#pragma output_port out_value
Int<8> out_value;

void hls_main() {
    Int<8> uninit;
    Int<8> copy = a;
    Int<8> direct(a);
    Int<8> value{};
    Pair pair{a, b};
    Pair designated{.hi = b};
    std::array<Int<8>, 2> cleared = {};
    std::array<Pair, 2> table = {};
    out_value = copy;
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

    auto records = pred::s0clang18::collectRecordMetadata(
        fixture.session, fixture.top, fixture.ports, fixture.type_context, policy);
    CHECK(records.ok());
    CHECK(records.value.has_value());
    fixture.records = std::move(*records.value);

    auto reachability = pred::s0clang18::collectFunctionReachability(
        fixture.session, fixture.top, fixture.semantic, fixture.const_eval, policy);
    CHECK(reachability.ok());
    CHECK(reachability.value.has_value());
    fixture.reachability = std::move(*reachability.value);

    auto templates = pred::s0clang18::resolveTemplateSpecializations(
        fixture.session, fixture.reachability, fixture.const_eval, policy);
    CHECK(templates.ok());
    CHECK(templates.value.has_value());
    fixture.templates = std::move(*templates.value);

    auto lambdas = pred::s0clang18::resolveLambdaCaptures(
        fixture.session, fixture.reachability, fixture.type_context, policy);
    CHECK(lambdas.ok());
    CHECK(lambdas.value.has_value());
    fixture.lambdas = std::move(*lambdas.value);
    return fixture;
}

static pred::s0clang18::ExprBuildContext makeContext(Fixture& fixture) {
    pred::s0clang18::ExprBuildContext context;
    context.session = &fixture.session;
    context.semantic_index = &fixture.semantic;
    context.type_context = &fixture.type_context;
    context.const_eval = &fixture.const_eval;
    context.records = &fixture.records;
    context.reachability = &fixture.reachability;
    context.templates = &fixture.templates;
    context.lambdas = &fixture.lambdas;
    context.loc_policy.canonicalize_paths = false;
    return context;
}

static pred::s0clang18::InitBuildResult buildInit(
    Fixture& fixture,
    const pred::s0clang18::ExprBuildContext& context,
    clang::VarDecl* decl) {
    CHECK(decl != nullptr);
    pred::DebugLoc loc = pred::s0clang18::debugLocForLocation(
        fixture.session, decl->getLocation(), context.loc_policy);
    auto lowered = pred::s0clang18::lowerQualType(
        fixture.type_context, decl->getType(), loc);
    if (!lowered.diagnostics.empty()) {
        for (const auto& diagnostic : lowered.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(lowered.diagnostics.empty());
    CHECK(lowered.value.has_value());

    pred::s0clang18::InitBuildInput input;
    input.decl = decl;
    input.init_expr = decl->getInit();
    input.target_type = lowered.value->type;
    input.loc = pred::s0clang18::debugLocForRange(
        fixture.session, decl->getSourceRange(), context.loc_policy);
    auto result = pred::s0clang18::buildInitializer(context, input);
    if (!result.diagnostics.empty()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.diagnostics.empty());
    return result;
}

static bool containsVar(const pred::v2::ExprPtr& expr, const std::string& name) {
    if (!expr) return false;
    if (expr->kind == pred::v2::ExprKind::VarRef && expr->var_name == name) {
        return true;
    }
    if (containsVar(expr->left, name) || containsVar(expr->right, name) ||
        containsVar(expr->operand, name) || containsVar(expr->array_base, name) ||
        containsVar(expr->index, name) || containsVar(expr->struct_base, name) ||
        containsVar(expr->cast_expr, name) || containsVar(expr->cond, name) ||
        containsVar(expr->then_expr, name) || containsVar(expr->else_expr, name) ||
        containsVar(expr->base, name) || containsVar(expr->value, name)) {
        return true;
    }
    for (const auto& arg : expr->args) {
        if (containsVar(arg, name)) return true;
    }
    for (const auto& part : expr->parts) {
        if (containsVar(part, name)) return true;
    }
    return false;
}

int main() {
    Fixture fixture = buildFixture();
    pred::s0clang18::ExprBuildContext context = makeContext(fixture);

    VarCollector collector;
    collector.TraverseDecl(fixture.session.translation_unit);

    auto uninit = buildInit(fixture, context, collector.vars.at("uninit"));
    CHECK(!pred::s0clang18::hasSyntacticInitializer(collector.vars.at("uninit")));
    CHECK(uninit.form == pred::s0clang18::InitForm::None);
    CHECK(!uninit.init_expr.has_value());
    CHECK(uninit.init_args.empty());
    CHECK(!uninit.default_constructed);

    auto copy = buildInit(fixture, context, collector.vars.at("copy"));
    CHECK(pred::s0clang18::hasSyntacticInitializer(collector.vars.at("copy")));
    CHECK(copy.form == pred::s0clang18::InitForm::Copy);
    CHECK(copy.init_expr.has_value());
    CHECK(copy.init_args.empty());

    auto direct = buildInit(fixture, context, collector.vars.at("direct"));
    CHECK(direct.form == pred::s0clang18::InitForm::Direct);
    CHECK(direct.init_expr.has_value());
    CHECK(direct.init_args.empty());

    auto value = buildInit(fixture, context, collector.vars.at("value"));
    CHECK(value.form == pred::s0clang18::InitForm::Aggregate ||
          value.form == pred::s0clang18::InitForm::Value ||
          value.form == pred::s0clang18::InitForm::List);
    CHECK(value.default_constructed);
    CHECK(value.init_args.size() == 1 || value.init_expr.has_value());

    auto pair = buildInit(fixture, context, collector.vars.at("pair"));
    CHECK(pair.form == pred::s0clang18::InitForm::Aggregate);
    CHECK(pair.init_args.size() == 2);
    CHECK(containsVar(pair.init_args[0], "a"));
    CHECK(containsVar(pair.init_args[1], "b"));

    auto designated = buildInit(fixture, context, collector.vars.at("designated"));
    CHECK(designated.form == pred::s0clang18::InitForm::Designated);
    CHECK(designated.init_args.size() == 2);
    CHECK(designated.init_args[0]->kind == pred::v2::ExprKind::Literal);
    CHECK(designated.init_args[0]->literal_value == "0");
    CHECK(containsVar(designated.init_args[1], "b"));

    auto cleared = buildInit(fixture, context, collector.vars.at("cleared"));
    CHECK(cleared.form == pred::s0clang18::InitForm::Aggregate);
    CHECK(cleared.init_args.size() == 2);
    CHECK(cleared.init_args[0]->kind == pred::v2::ExprKind::Literal);
    CHECK(cleared.init_args[1]->kind == pred::v2::ExprKind::Literal);

    auto table = buildInit(fixture, context, collector.vars.at("table"));
    CHECK(table.form == pred::s0clang18::InitForm::Aggregate);
    CHECK(table.init_args.size() == 4);
    for (const auto& arg : table.init_args) {
        CHECK(arg != nullptr);
        CHECK(arg->kind == pred::v2::ExprKind::Literal);
        CHECK(arg->literal_value == "0");
    }

    std::cout << "s014init_test passed\n";
    return 0;
}
