#include "s0clang18/s009reachability.hpp"

#include <clang/AST/ASTLambda.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const Clang18Session& session,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.9";
    diagnostic.context.source_file = session.main_file_path;
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

bool hasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

const clang::FunctionDecl* canonicalFunction(const clang::FunctionDecl* decl) {
    if (!decl) return nullptr;
    return llvm::dyn_cast<clang::FunctionDecl>(decl->getCanonicalDecl());
}

const clang::FunctionDecl* functionDefinition(const clang::FunctionDecl* decl) {
    if (!decl) return nullptr;
    if (const clang::FunctionDecl* definition = decl->getDefinition()) {
        return definition;
    }
    return decl;
}

std::string sanitizeName(std::string name) {
    if (name.empty()) return "anonymous";
    for (char& ch : name) {
        unsigned char value = static_cast<unsigned char>(ch);
        if (!std::isalnum(value) && ch != '_') ch = '_';
    }
    return name;
}

std::string qualifiedFunctionName(const clang::FunctionDecl* function) {
    if (!function) return "unknown";
    if (const auto* named = llvm::dyn_cast<clang::NamedDecl>(function)) {
        std::string qualified = named->getQualifiedNameAsString();
        if (!qualified.empty()) return qualified;
        std::string name = named->getNameAsString();
        if (!name.empty()) return name;
    }
    return "anonymous";
}

bool isFunctionTemplateSpecialization(const clang::FunctionDecl* function) {
    return function && function->getTemplatedKind() ==
                           clang::FunctionDecl::TK_FunctionTemplateSpecialization;
}

std::optional<FunctionEntityKind> classifyReachableFunction(
    const clang::FunctionDecl* function,
    const TopFunctionSelection& top) {
    if (!function) return std::nullopt;

    const clang::FunctionDecl* canonical = canonicalFunction(function);
    const clang::FunctionDecl* top_canonical = canonicalFunction(top.function_decl);
    if (canonical && top_canonical && canonical == top_canonical) {
        return FunctionEntityKind::Top;
    }

    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function)) {
        if (clang::isLambdaCallOperator(method)) {
            if (isFunctionTemplateSpecialization(method)) {
                return FunctionEntityKind::GenericLambdaSpecialization;
            }
            return FunctionEntityKind::Lambda;
        }
        return FunctionEntityKind::Method;
    }

    if (isFunctionTemplateSpecialization(function)) {
        return FunctionEntityKind::FunctionTemplateSpecialization;
    }

    return FunctionEntityKind::Helper;
}

std::vector<long long> collectTemplateValues(const ConstEvalContext& const_eval,
                                             const clang::FunctionDecl* function,
                                             DebugLoc loc,
                                             std::vector<Diagnostic>& diagnostics) {
    std::vector<long long> values;
    if (!function) return values;

    const clang::TemplateArgumentList* args =
        function->getTemplateSpecializationArgs();
    if (!args) return values;

    for (const clang::TemplateArgument& arg : args->asArray()) {
        if (arg.getKind() != clang::TemplateArgument::Integral) continue;
        auto value = evalTemplateIntegralArgument(const_eval, arg, loc);
        if (!value.ok()) {
            diagnostics.insert(diagnostics.end(),
                               value.diagnostics.begin(),
                               value.diagnostics.end());
            continue;
        }
        if (value.value) values.push_back(*value.value);
    }
    return values;
}

std::string stableNameFor(const clang::FunctionDecl* function,
                          FunctionEntityKind kind,
                          const std::vector<long long>& template_values,
                          const DebugLoc& loc) {
    std::ostringstream os;
    switch (kind) {
    case FunctionEntityKind::Top:
        os << "top_";
        break;
    case FunctionEntityKind::Helper:
        os << "helper_";
        break;
    case FunctionEntityKind::FunctionTemplateSpecialization:
        os << "template_";
        break;
    case FunctionEntityKind::Lambda:
        os << "lambda_";
        break;
    case FunctionEntityKind::GenericLambdaSpecialization:
        os << "generic_lambda_";
        break;
    case FunctionEntityKind::Method:
        os << "method_";
        break;
    }

    if (kind == FunctionEntityKind::Lambda ||
        kind == FunctionEntityKind::GenericLambdaSpecialization) {
        os << "at_" << loc.line << "_" << loc.column;
    } else {
        os << sanitizeName(qualifiedFunctionName(function));
    }

    if (!template_values.empty()) {
        os << "_T";
        for (long long value : template_values) os << "_" << value;
    }
    return os.str();
}

const clang::FunctionDecl* normalizeReachableCallee(const clang::FunctionDecl* callee) {
    if (!callee) return nullptr;
    return functionDefinition(callee);
}

const clang::FunctionDecl* directCallee(const clang::CallExpr* expr) {
    if (!expr) return nullptr;
    if (const auto* member_call = llvm::dyn_cast<clang::CXXMemberCallExpr>(expr)) {
        if (const clang::CXXMethodDecl* method = member_call->getMethodDecl()) {
            return method;
        }
    }
    return expr->getDirectCallee();
}

class CallCollector : public clang::RecursiveASTVisitor<CallCollector> {
public:
    explicit CallCollector(const Clang18Session& session,
                           SourceLocPolicy loc_policy)
        : session_(session), loc_policy_(std::move(loc_policy)) {}

    bool shouldVisitLambdaBody() const { return false; }

    bool TraverseLambdaExpr(clang::LambdaExpr*) {
        return true;
    }

    bool VisitCallExpr(clang::CallExpr* expr) {
        if (!expr) return true;
        const clang::FunctionDecl* callee = normalizeReachableCallee(directCallee(expr));
        if (!callee) return true;

        PendingCall call;
        call.callee = callee;
        call.expr = expr;
        call.loc = debugLocForRange(session_, expr->getSourceRange(), loc_policy_);
        calls.push_back(std::move(call));
        return true;
    }

    struct PendingCall {
        const clang::FunctionDecl* callee = nullptr;
        const clang::Expr* expr = nullptr;
        DebugLoc loc;
    };

    std::vector<PendingCall> calls;

private:
    const Clang18Session& session_;
    SourceLocPolicy loc_policy_;
};

class ReachabilityBuilder {
public:
    ReachabilityBuilder(const Clang18Session& session,
                        const TopFunctionSelection& top,
                        const SemanticIndex& semantic_index,
                        const ConstEvalContext& const_eval,
                        SourceLocPolicy loc_policy)
        : session_(session),
          top_(top),
          semantic_index_(semantic_index),
          const_eval_(const_eval),
          loc_policy_(std::move(loc_policy)) {}

    StepResult<FunctionReachabilityGraph> run() {
        StepResult<FunctionReachabilityGraph> result;
        addFunction(functionDefinition(top_.function_decl), FunctionEntityKind::Top,
                    top_.loc, result.diagnostics);

        std::size_t queue_index = 0;
        while (queue_index < worklist_.size()) {
            SemanticEntityId caller_id = worklist_[queue_index++];
            const FunctionEntity* caller = findFunctionEntity(graph_, caller_id);
            if (!caller || !caller->function_decl) continue;

            const clang::FunctionDecl* definition =
                functionDefinition(caller->function_decl);
            if (!definition || !definition->hasBody()) {
                result.diagnostics.push_back(makeError(
                    session_, caller->loc,
                    "Reachable function '" +
                        qualifiedFunctionName(caller->function_decl) +
                        "' does not have a definition"));
                continue;
            }

            CallCollector collector(session_, loc_policy_);
            collector.TraverseStmt(definition->getBody());
            for (const auto& call : collector.calls) {
                handleCall(caller_id, call, result.diagnostics);
            }
        }

        if (hasError(result.diagnostics)) return result;
        result.value = std::move(graph_);
        return result;
    }

private:
    bool isUserReachableCallee(const clang::FunctionDecl* callee) const {
        if (!callee) return false;
        if (canonicalFunction(callee) == canonicalFunction(top_.function_decl)) {
            return true;
        }
        if (findEntity(semantic_index_, callee)) return true;
        if (const clang::FunctionTemplateDecl* primary = callee->getPrimaryTemplate()) {
            if (findEntity(semantic_index_, primary)) return true;
            if (findEntity(semantic_index_, primary->getTemplatedDecl())) return true;
        }
        return false;
    }

    SemanticEntityId addFunction(const clang::FunctionDecl* function,
                                 FunctionEntityKind forced_kind,
                                 DebugLoc loc,
                                 std::vector<Diagnostic>& diagnostics) {
        function = functionDefinition(function);
        const clang::FunctionDecl* canonical = canonicalFunction(function);
        if (!canonical) canonical = function;

        auto existing = graph_.function_by_decl.find(canonical);
        if (existing != graph_.function_by_decl.end()) return existing->second;
        existing = graph_.function_by_decl.find(function);
        if (existing != graph_.function_by_decl.end()) return existing->second;

        std::optional<FunctionEntityKind> classified =
            classifyReachableFunction(function, top_);
        FunctionEntityKind kind = forced_kind;
        if (forced_kind != FunctionEntityKind::Top && classified) {
            kind = *classified;
        }

        std::vector<long long> template_values =
            collectTemplateValues(const_eval_, function, loc, diagnostics);
        std::string stable_name = stableNameFor(function, kind, template_values, loc);

        FunctionEntity entity;
        entity.id = static_cast<SemanticEntityId>(graph_.functions.size());
        entity.kind = kind;
        entity.key.function_decl = function;
        entity.key.template_values = std::move(template_values);
        entity.key.stable_name = stable_name;
        entity.function_decl = function;
        entity.loc = loc.valid()
            ? loc
            : debugLocForRange(session_, function->getSourceRange(), loc_policy_);

        graph_.function_by_decl.emplace(function, entity.id);
        if (canonical) graph_.function_by_decl.emplace(canonical, entity.id);
        graph_.function_by_stable_name.emplace(stable_name, entity.id);

        if (entity.kind == FunctionEntityKind::Top) {
            graph_.top_function = entity.id;
        }

        worklist_.push_back(entity.id);
        graph_.functions.push_back(std::move(entity));
        return graph_.functions.back().id;
    }

    void handleCall(SemanticEntityId caller_id,
                    const CallCollector::PendingCall& call,
                    std::vector<Diagnostic>& diagnostics) {
        if (!isUserReachableCallee(call.callee)) return;

        std::optional<FunctionEntityKind> kind =
            classifyReachableFunction(call.callee, top_);
        if (!kind) return;

        SemanticEntityId callee_id =
            addFunction(call.callee, *kind, call.loc, diagnostics);

        FunctionCallEdge edge;
        edge.caller = caller_id;
        edge.callee = callee_id;
        edge.call_expr = call.expr;
        edge.loc = call.loc;
        graph_.call_edges.push_back(std::move(edge));
    }

    const Clang18Session& session_;
    const TopFunctionSelection& top_;
    const SemanticIndex& semantic_index_;
    const ConstEvalContext& const_eval_;
    SourceLocPolicy loc_policy_;
    FunctionReachabilityGraph graph_;
    std::vector<SemanticEntityId> worklist_;
};

} // namespace

StepResult<FunctionReachabilityGraph> collectFunctionReachability(
    const Clang18Session& session,
    const TopFunctionSelection& top,
    const SemanticIndex& semantic_index,
    const ConstEvalContext& const_eval,
    const SourceLocPolicy& loc_policy) {
    StepResult<FunctionReachabilityGraph> result;
    if (!session.ast_context || !session.translation_unit) {
        result.diagnostics.push_back(makeError(
            session, {}, "S0Clang18 reachability requires a valid Clang18Session"));
        return result;
    }
    if (!top.function_decl) {
        result.diagnostics.push_back(makeError(
            session, top.loc, "Function reachability requires a selected top function"));
        return result;
    }

    ReachabilityBuilder builder(session, top, semantic_index, const_eval, loc_policy);
    return builder.run();
}

const FunctionEntity* findFunctionEntity(const FunctionReachabilityGraph& graph,
                                         SemanticEntityId id) {
    if (id < 0 || static_cast<std::size_t>(id) >= graph.functions.size()) {
        return nullptr;
    }
    return &graph.functions[static_cast<std::size_t>(id)];
}

} // namespace pred::s0clang18
