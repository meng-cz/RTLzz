#include "s0clang18/s012expr.hpp"

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
    options.source_name = "testv2/clang18/s012_virtual_input.logic.cpp";
    options.source_text = R"cpp(
#include <fixint.hpp>

struct Pair {
    Int<8> lo;
    Int<8> hi;
};

Int<8> helper(Int<8> x) {
    return x ^ Int<8>(5);
}

#pragma input_port a
Int<8> a;
#pragma input_port b
Int<8> b;
#pragma input_port sel
bool sel;
#pragma output_port out_value
Int<8> out_value;

void hls_main() {
    Int<9> sum = a + b;
    Int<8> narrowed = Int<8>(sum);
    bool flag = sel ? true : false;
    Pair pair{narrowed, b};
    Int<8> field = pair.lo;
    Int<8> call_value = helper(field);
    Int<8> indexed = (&pair.lo)[0];
    out_value = flag ? call_value : indexed;
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

static pred::v2::ExprPtr buildRequired(const pred::s0clang18::ExprBuildContext& context,
                                       clang::VarDecl* var) {
    CHECK(var != nullptr);
    CHECK(var->getInit() != nullptr);
    auto result = pred::s0clang18::buildExpr(context, var->getInit());
    if (!result.diagnostics.empty()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.diagnostics.empty());
    CHECK(result.expr != nullptr);
    CHECK(result.expr->debug_loc.valid());
    return result.expr;
}

static bool containsKind(const pred::v2::ExprPtr& expr, pred::v2::ExprKind kind) {
    if (!expr) return false;
    if (expr->kind == kind) return true;
    if (containsKind(expr->left, kind) || containsKind(expr->right, kind) ||
        containsKind(expr->operand, kind) || containsKind(expr->array_base, kind) ||
        containsKind(expr->index, kind) || containsKind(expr->struct_base, kind) ||
        containsKind(expr->cast_expr, kind) || containsKind(expr->cond, kind) ||
        containsKind(expr->then_expr, kind) || containsKind(expr->else_expr, kind) ||
        containsKind(expr->base, kind) || containsKind(expr->value, kind)) {
        return true;
    }
    for (const auto& arg : expr->args) {
        if (containsKind(arg, kind)) return true;
    }
    for (const auto& part : expr->parts) {
        if (containsKind(part, kind)) return true;
    }
    return false;
}

static bool containsField(const pred::v2::ExprPtr& expr, const std::string& field) {
    if (!expr) return false;
    if (expr->kind == pred::v2::ExprKind::FieldAccess && expr->field_name == field) {
        return true;
    }
    if (containsField(expr->left, field) || containsField(expr->right, field) ||
        containsField(expr->operand, field) || containsField(expr->array_base, field) ||
        containsField(expr->index, field) || containsField(expr->struct_base, field) ||
        containsField(expr->cast_expr, field) || containsField(expr->cond, field) ||
        containsField(expr->then_expr, field) || containsField(expr->else_expr, field) ||
        containsField(expr->base, field) || containsField(expr->value, field)) {
        return true;
    }
    for (const auto& arg : expr->args) {
        if (containsField(arg, field)) return true;
    }
    for (const auto& part : expr->parts) {
        if (containsField(part, field)) return true;
    }
    return false;
}

int main() {
    Fixture fixture = buildFixture();
    pred::s0clang18::ExprBuildContext context = makeContext(fixture);

    VarCollector collector;
    collector.TraverseDecl(fixture.session.translation_unit);

    auto sum = buildRequired(context, collector.vars.at("sum"));
    CHECK(sum->kind == pred::v2::ExprKind::BinaryOp);
    CHECK(sum->op == "+");
    CHECK(sum->type.width == 9);

    auto narrowed = buildRequired(context, collector.vars.at("narrowed"));
    CHECK(narrowed->kind == pred::v2::ExprKind::Cast);
    CHECK(narrowed->cast_expr != nullptr);
    CHECK(narrowed->type.width == 8);

    auto flag = buildRequired(context, collector.vars.at("flag"));
    CHECK(flag->kind == pred::v2::ExprKind::Ternary);
    CHECK(flag->type.hw_kind == "bool");

    auto pair = buildRequired(context, collector.vars.at("pair"));
    CHECK(pair->kind == pred::v2::ExprKind::Call);
    CHECK(pair->callee == "__init_list");
    CHECK(pair->args.size() == 2);

    auto field = buildRequired(context, collector.vars.at("field"));
    CHECK(containsField(field, "lo"));
    CHECK(field->type.width == 8);

    auto call = buildRequired(context, collector.vars.at("call_value"));
    CHECK(call->kind == pred::v2::ExprKind::Call);
    CHECK(call->callee.find("helper") != std::string::npos);
    CHECK(call->args.size() == 1);

    auto indexed = buildRequired(context, collector.vars.at("indexed"));
    CHECK(containsKind(indexed, pred::v2::ExprKind::ArrayAccess) ||
          containsKind(indexed, pred::v2::ExprKind::UnaryOp));

    CHECK(pred::s0clang18::binaryOpcodeForClangExpr(nullptr).empty());
    CHECK(pred::s0clang18::unaryOpcodeForClangExpr(nullptr).empty());

    std::cout << "s012expr_test passed\n";
    return 0;
}
