#include "s0clang18/s013stmt.hpp"

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
    pred::s0clang18::RecordMetadataSet records;
    pred::s0clang18::FunctionReachabilityGraph reachability;
    pred::s0clang18::TemplateSpecializationTable templates;
    pred::s0clang18::LambdaCaptureTable lambdas;
};

static Fixture buildFixture() {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s013_virtual_input.logic.cpp";
    options.source_text = R"cpp(
#include <cstdint>
#include <fixint.hpp>

Int<8> helper(Int<8> x, bool sel) {
    Int<8> ret;
    if (sel) {
        ret = Int<8>(x + Int<8>(1));
    } else {
        ret = x ^ Int<8>(3);
    }
    return ret;
}

#pragma input_port a
Int<8> a;
#pragma input_port sel
bool sel;
#pragma input_port mode
uint8_t mode;
#pragma output_port out_value
Int<8> out_value;

void hls_main() {
    Int<8> acc;
    acc = a;
    auto local_lambda = [](Int<8> x) {
        return x;
    };
    acc = local_lambda(acc);
    helper(acc, sel);

    for (uint32_t i = 0; i < 3; i = i + 1) {
        if (sel) {
            continue;
        }
        acc = Int<8>(acc + Int<8>(1));
    }

    while (sel) {
        acc = acc ^ Int<8>(2);
        break;
    }

    do {
        acc = acc ^ Int<8>(4);
    } while (false);

    switch (mode) {
    case 0:
        acc = acc ^ Int<8>(5);
        break;
    case 1:
        acc = Int<8>(acc + Int<8>(6));
        break;
    default:
        acc = acc ^ Int<8>(7);
        break;
    }

    out_value = acc;
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

static pred::s0clang18::StmtBuildContext makeContext(Fixture& fixture,
                                                     pred::s0clang18::SemanticEntityId id) {
    pred::s0clang18::StmtBuildContext context;
    context.current_function = id;
    context.expr_context.session = &fixture.session;
    context.expr_context.semantic_index = &fixture.semantic;
    context.expr_context.type_context = &fixture.type_context;
    context.expr_context.const_eval = &fixture.const_eval;
    context.expr_context.records = &fixture.records;
    context.expr_context.reachability = &fixture.reachability;
    context.expr_context.templates = &fixture.templates;
    context.expr_context.lambdas = &fixture.lambdas;
    context.expr_context.loc_policy.canonicalize_paths = false;
    return context;
}

static int countKind(const std::vector<pred::v2::StmtPtr>& stmts,
                     pred::v2::StmtKind kind) {
    int count = 0;
    for (const auto& stmt : stmts) {
        if (!stmt) continue;
        if (stmt->kind == kind) ++count;
    }
    return count;
}

static bool containsKind(const std::vector<pred::v2::StmtPtr>& stmts,
                         pred::v2::StmtKind kind) {
    for (const auto& stmt : stmts) {
        if (!stmt) continue;
        if (stmt->kind == kind) return true;
        if (containsKind(stmt->if_then, kind) ||
            containsKind(stmt->if_else, kind) ||
            containsKind(stmt->for_body, kind) ||
            containsKind(stmt->while_body, kind) ||
            containsKind(stmt->block_stmts, kind)) {
            return true;
        }
        if (stmt->for_init && stmt->for_init->kind == kind) return true;
        for (const auto& clause : stmt->switch_cases) {
            if (containsKind(clause.body, kind)) return true;
        }
    }
    return false;
}

static const pred::v2::StmtPtr* findFirst(
    const std::vector<pred::v2::StmtPtr>& stmts,
    pred::v2::StmtKind kind) {
    for (const auto& stmt : stmts) {
        if (stmt && stmt->kind == kind) return &stmt;
    }
    return nullptr;
}

int main() {
    Fixture fixture = buildFixture();
    auto top_context = makeContext(fixture, fixture.reachability.top_function);
    auto top_body = pred::s0clang18::buildFunctionBody(
        top_context, fixture.top.function_decl);
    if (!top_body.diagnostics.empty()) {
        for (const auto& diagnostic : top_body.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(top_body.diagnostics.empty());
    CHECK(!top_body.stmts.empty());

    CHECK(countKind(top_body.stmts, pred::v2::StmtKind::Decl) >= 1);
    CHECK(countKind(top_body.stmts, pred::v2::StmtKind::Assign) >= 2);
    CHECK(containsKind(top_body.stmts, pred::v2::StmtKind::ExprStmt));
    CHECK(containsKind(top_body.stmts, pred::v2::StmtKind::If));
    CHECK(containsKind(top_body.stmts, pred::v2::StmtKind::For));
    CHECK(containsKind(top_body.stmts, pred::v2::StmtKind::While));
    CHECK(containsKind(top_body.stmts, pred::v2::StmtKind::DoWhile));
    CHECK(containsKind(top_body.stmts, pred::v2::StmtKind::Switch));
    CHECK(containsKind(top_body.stmts, pred::v2::StmtKind::Break));
    CHECK(containsKind(top_body.stmts, pred::v2::StmtKind::Continue));

    const auto* decl = findFirst(top_body.stmts, pred::v2::StmtKind::Decl);
    CHECK(decl != nullptr);
    CHECK((*decl)->decl_name == "acc");
    CHECK(!(*decl)->decl_init.has_value());
    CHECK(!(*decl)->decl_default_constructed);

    bool saw_lambda_decl = false;
    for (const auto& stmt : top_body.stmts) {
        if (stmt && stmt->kind == pred::v2::StmtKind::Decl &&
            stmt->decl_name == "local_lambda") {
            saw_lambda_decl = true;
        }
    }
    CHECK(!saw_lambda_decl);

    const auto* for_stmt = findFirst(top_body.stmts, pred::v2::StmtKind::For);
    CHECK(for_stmt != nullptr);
    CHECK((*for_stmt)->for_init != nullptr);
    CHECK((*for_stmt)->for_cond != nullptr);
    CHECK((*for_stmt)->for_step != nullptr);
    CHECK(!(*for_stmt)->for_body.empty());

    const auto* switch_stmt = findFirst(top_body.stmts, pred::v2::StmtKind::Switch);
    CHECK(switch_stmt != nullptr);
    CHECK((*switch_stmt)->switch_cases.size() >= 3);

    pred::s0clang18::SemanticEntityId helper_id = -1;
    for (const auto& function : fixture.reachability.functions) {
        if (function.key.stable_name.find("helper") != std::string::npos) {
            helper_id = function.id;
        }
    }
    CHECK(helper_id >= 0);
    const auto* helper_fn =
        pred::s0clang18::findFunctionEntity(fixture.reachability, helper_id);
    CHECK(helper_fn != nullptr);
    auto helper_body = pred::s0clang18::buildFunctionBody(
        makeContext(fixture, helper_id), helper_fn->function_decl);
    CHECK(helper_body.diagnostics.empty());
    CHECK(containsKind(helper_body.stmts, pred::v2::StmtKind::Return));

    std::cout << "s013stmt_test passed\n";
    return 0;
}
