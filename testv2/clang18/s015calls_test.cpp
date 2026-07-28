#include "s0clang18/s015calls.hpp"

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

class CallFinder : public clang::RecursiveASTVisitor<CallFinder> {
public:
    explicit CallFinder(std::string expected_name = {})
        : expected_name_(std::move(expected_name)) {}

    bool VisitCallExpr(clang::CallExpr* expr) {
        if (first || !expr) return true;
        if (expected_name_.empty()) {
            first = expr;
            return true;
        }
        const clang::FunctionDecl* callee = expr->getDirectCallee();
        if (const auto* member = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
            if (const clang::CXXMethodDecl* method = member->getMethodDecl()) {
                callee = method;
            }
        }
        if (!callee) return true;
        std::string name = callee->getNameAsString();
        if (name == expected_name_) first = expr;
        return true;
    }

    clang::CallExpr* first = nullptr;

private:
    std::string expected_name_;
};

static Fixture buildFixture() {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s015_virtual_input.logic.cpp";
    options.source_text = R"cpp(
#include <array>
#include <fixint.hpp>

Int<8> helper(Int<8> x) {
    return x;
}

template <int N>
Int<8> templated(Int<8> x) {
    return x.template at<N - 1, 0>();
}

struct Worker {
    template <int K>
    Int<8> bump(Int<8> x) {
        return templated<K + 1>(x);
    }
};

#pragma input_port a
Int<8> a;
#pragma input_port b
Int<8> b;
#pragma output_port out_value
Int<8> out_value;

void hls_main() {
    Worker worker{};
    auto local_lambda = [](Int<8> x) {
        return helper(x);
    };
    auto generic_lambda = []<int L>(Int<8> x) {
        return templated<L>(x);
    };
    Int<8> helper_result = helper(a);
    Int<8> template_result = templated<4>(a);
    Int<8> member_result = worker.template bump<2>(a);
    Int<8> lambda_result = local_lambda(a);
    Int<8> generic_result = generic_lambda.template operator()<5>(a);
    Int<8> slice_result = a.template at<7, 0>();
    Int<16> cat_result = Cat(a, b);
    Int<8> op_result = a + b;
    std::array<Int<8>, 2> buffer = {};
    auto unsupported_result = buffer.size();
    out_value = helper_result ^ template_result ^ member_result ^ lambda_result ^
                generic_result ^ slice_result ^ op_result;
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

static clang::CallExpr* firstCallIn(clang::VarDecl* var,
                                    const std::string& callee_name) {
    CHECK(var != nullptr);
    CHECK(var->getInit() != nullptr);
    CallFinder finder(callee_name);
    finder.TraverseStmt(var->getInit());
    CHECK(finder.first != nullptr);
    return finder.first;
}

static pred::s0clang18::BoundCall bindRequired(
    const pred::s0clang18::ExprBuildContext& context,
    clang::CallExpr* call) {
    auto result = pred::s0clang18::bindCallExpr(context, call);
    if (!result.diagnostics.empty()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.diagnostics.empty());
    CHECK(result.call.has_value());
    CHECK(result.call->loc.valid());
    return *result.call;
}

static bool hasTemplateValue(const pred::s0clang18::BoundCall& call,
                             long long expected) {
    for (long long value : call.template_values) {
        if (value == expected) return true;
    }
    return false;
}

int main() {
    Fixture fixture = buildFixture();
    auto context = makeContext(fixture);

    VarCollector collector;
    collector.TraverseDecl(fixture.session.translation_unit);

    auto helper = bindRequired(
        context, firstCallIn(collector.vars.at("helper_result"), "helper"));
    CHECK(helper.kind == pred::s0clang18::BoundCallKind::Helper);
    CHECK(helper.function_id >= 0);
    CHECK(helper.stable_callee_name.find("helper") != std::string::npos);

    auto templated = bindRequired(
        context, firstCallIn(collector.vars.at("template_result"), "templated"));
    CHECK(templated.kind == pred::s0clang18::BoundCallKind::TemplateHelper);
    CHECK(templated.function_id >= 0);
    CHECK(hasTemplateValue(templated, 4));

    auto member = bindRequired(
        context, firstCallIn(collector.vars.at("member_result"), "bump"));
    CHECK(member.kind == pred::s0clang18::BoundCallKind::MemberHelper);
    CHECK(member.receiver_expr != nullptr);
    CHECK(hasTemplateValue(member, 2));

    auto lambda = bindRequired(
        context, firstCallIn(collector.vars.at("lambda_result"), "operator()"));
    CHECK(lambda.kind == pred::s0clang18::BoundCallKind::Lambda);
    CHECK(lambda.function_id >= 0);

    auto generic = bindRequired(
        context, firstCallIn(collector.vars.at("generic_result"), "operator()"));
    CHECK(generic.kind == pred::s0clang18::BoundCallKind::GenericLambda);
    CHECK(generic.function_id >= 0);
    CHECK(hasTemplateValue(generic, 5));

    auto slice = bindRequired(
        context, firstCallIn(collector.vars.at("slice_result"), "at"));
    CHECK(slice.kind == pred::s0clang18::BoundCallKind::FixintAPI);
    CHECK(slice.api_name == "at");
    CHECK(slice.receiver_expr != nullptr);
    CHECK(slice.template_values.size() == 2);
    CHECK(slice.template_values[0] == 7);
    CHECK(slice.template_values[1] == 0);

    auto cat = bindRequired(context, firstCallIn(collector.vars.at("cat_result"), "Cat"));
    CHECK(cat.kind == pred::s0clang18::BoundCallKind::FixintAPI);
    CHECK(cat.api_name == "Cat");

    clang::CallExpr* op_call = firstCallIn(collector.vars.at("op_result"), "operator+");
    auto op_result = pred::s0clang18::bindCXXOperatorCallExpr(
        context, llvm::dyn_cast<clang::CXXOperatorCallExpr>(op_call));
    CHECK(op_result.diagnostics.empty());
    CHECK(op_result.call.has_value());
    CHECK(op_result.call->kind == pred::s0clang18::BoundCallKind::FixintAPI);
    CHECK(op_result.call->api_name == "+");

    auto unsupported = pred::s0clang18::bindCallExpr(
        context, firstCallIn(collector.vars.at("unsupported_result"), "size"));
    CHECK(!unsupported.diagnostics.empty());
    CHECK(unsupported.call.has_value());
    CHECK(unsupported.call->kind == pred::s0clang18::BoundCallKind::Unsupported);

    std::cout << "s015calls_test passed\n";
    return 0;
}
