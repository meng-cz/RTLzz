#include "s0clang18/s014init.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>

#include <algorithm>
#include <optional>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const ExprBuildContext& context,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.14";
    if (context.session) diagnostic.context.source_file = context.session->main_file_path;
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

bool hasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

DebugLoc exprLoc(const ExprBuildContext& context, const clang::Expr* expr) {
    if (!context.session || !expr) return {};
    return debugLocForRange(*context.session, expr->getSourceRange(),
                            context.loc_policy);
}

const clang::Expr* unwrapTransparentExpr(const clang::Expr* expr) {
    while (expr) {
        if (const auto* cleanup = llvm::dyn_cast<clang::ExprWithCleanups>(expr)) {
            expr = cleanup->getSubExpr();
            continue;
        }
        if (const auto* materialized =
                llvm::dyn_cast<clang::MaterializeTemporaryExpr>(expr)) {
            expr = materialized->getSubExpr();
            continue;
        }
        if (const auto* bind = llvm::dyn_cast<clang::CXXBindTemporaryExpr>(expr)) {
            expr = bind->getSubExpr();
            continue;
        }
        if (const auto* paren = llvm::dyn_cast<clang::ParenExpr>(expr)) {
            expr = paren->getSubExpr();
            continue;
        }
        if (const auto* implicit = llvm::dyn_cast<clang::ImplicitCastExpr>(expr)) {
            expr = implicit->getSubExpr();
            continue;
        }
        if (const auto* constant = llvm::dyn_cast<clang::ConstantExpr>(expr)) {
            expr = constant->getSubExpr();
            continue;
        }
        break;
    }
    return expr;
}

const clang::InitListExpr* asInitList(const clang::Expr* expr) {
    expr = unwrapTransparentExpr(expr);
    return llvm::dyn_cast_or_null<clang::InitListExpr>(expr);
}

bool containsDesignatedInit(const clang::Expr* expr) {
    expr = unwrapTransparentExpr(expr);
    if (!expr) return false;
    if (llvm::isa<clang::DesignatedInitExpr>(expr)) return true;
    if (const auto* list = llvm::dyn_cast<clang::InitListExpr>(expr)) {
        if (list->hasDesignatedInit()) return true;
        if (const clang::InitListExpr* syntactic = list->getSyntacticForm()) {
            if (syntactic->hasDesignatedInit()) return true;
        }
    }
    for (const clang::Stmt* child : expr->children()) {
        if (containsDesignatedInit(llvm::dyn_cast_or_null<clang::Expr>(child))) {
            return true;
        }
    }
    return false;
}

pred::v2::TypeInfo scalarElementType(pred::v2::TypeInfo type) {
    type.is_array = false;
    type.array_size = 0;
    type.array_dims.clear();
    return type;
}

const RecordMetadata* recordForType(const ExprBuildContext& context,
                                    const pred::v2::TypeInfo& type) {
    if (!context.records) return nullptr;
    if (!type.struct_name.empty()) {
        auto found = context.records->record_by_name.find(type.struct_name);
        if (found != context.records->record_by_name.end() &&
            found->second < context.records->records.size()) {
            return &context.records->records[found->second];
        }
    }
    if (!type.name.empty()) {
        auto found = context.records->record_by_name.find(type.name);
        if (found != context.records->record_by_name.end() &&
            found->second < context.records->records.size()) {
            return &context.records->records[found->second];
        }
    }
    return nullptr;
}

std::size_t leafCountForType(const ExprBuildContext& context,
                             const pred::v2::TypeInfo& type);

std::size_t leafCountForRecord(const ExprBuildContext& context,
                               const RecordMetadata& record) {
    std::size_t count = 0;
    for (const RecordField& field : record.fields) {
        count += leafCountForType(context, field.type);
    }
    return count;
}

std::size_t leafCountForType(const ExprBuildContext& context,
                             const pred::v2::TypeInfo& type) {
    if (type.is_array) {
        std::size_t elements = 1;
        for (int dim : type.array_dims) {
            if (dim <= 0) return 0;
            elements *= static_cast<std::size_t>(dim);
        }
        return elements * leafCountForType(context, scalarElementType(type));
    }
    if (const RecordMetadata* record = recordForType(context, type)) {
        return leafCountForRecord(context, *record);
    }
    return 1;
}

void appendLeafTypes(const ExprBuildContext& context,
                     const pred::v2::TypeInfo& type,
                     std::vector<pred::v2::TypeInfo>& out) {
    if (type.is_array) {
        std::size_t elements = 1;
        for (int dim : type.array_dims) {
            if (dim <= 0) return;
            elements *= static_cast<std::size_t>(dim);
        }
        pred::v2::TypeInfo elem = scalarElementType(type);
        for (std::size_t i = 0; i < elements; ++i) {
            appendLeafTypes(context, elem, out);
        }
        return;
    }
    if (const RecordMetadata* record = recordForType(context, type)) {
        for (const RecordField& field : record->fields) {
            appendLeafTypes(context, field.type, out);
        }
        return;
    }
    out.push_back(type);
}

std::size_t leafCountForExpr(const ExprBuildContext& context,
                             const pred::v2::ExprPtr& expr) {
    if (!expr) return 0;
    std::size_t count = leafCountForType(context, expr->type);
    return count == 0 ? 1 : count;
}

std::size_t leafCountForExprList(const ExprBuildContext& context,
                                 const std::vector<pred::v2::ExprPtr>& exprs) {
    std::size_t count = 0;
    for (const auto& expr : exprs) count += leafCountForExpr(context, expr);
    return count;
}

pred::v2::ExprPtr defaultValueForScalar(const pred::v2::TypeInfo& type,
                                        DebugLoc loc) {
    pred::v2::ExprPtr expr;
    if (type.name == "bool" || type.hw_kind == "bool") {
        expr = pred::v2::make_literal("false", pred::v2::make_bool_type());
    } else if (type.width > 0 || type.is_hw_int) {
        expr = pred::v2::make_literal("0", type);
    } else if (!type.struct_name.empty() || !type.name.empty()) {
        auto call = std::make_shared<pred::v2::Expr>();
        call->kind = pred::v2::ExprKind::Call;
        call->callee = type.struct_name.empty() ? type.name : type.struct_name;
        call->type = type;
        expr = std::move(call);
    } else {
        expr = pred::v2::make_literal("0", type);
    }
    if (expr) expr->debug_loc = std::move(loc);
    return expr;
}

void appendDefaultLeaves(const ExprBuildContext& context,
                         const pred::v2::TypeInfo& type,
                         DebugLoc loc,
                         std::vector<pred::v2::ExprPtr>& out) {
    if (type.is_array) {
        std::size_t elements = 1;
        for (int dim : type.array_dims) {
            if (dim <= 0) return;
            elements *= static_cast<std::size_t>(dim);
        }
        pred::v2::TypeInfo elem = scalarElementType(type);
        for (std::size_t i = 0; i < elements; ++i) {
            appendDefaultLeaves(context, elem, loc, out);
        }
        return;
    }
    if (const RecordMetadata* record = recordForType(context, type)) {
        for (const RecordField& field : record->fields) {
            appendDefaultLeaves(context, field.type, loc, out);
        }
        return;
    }
    out.push_back(defaultValueForScalar(type, loc));
}

std::vector<pred::v2::TypeInfo> childTypesForAggregate(
    const ExprBuildContext& context,
    const pred::v2::TypeInfo& type) {
    std::vector<pred::v2::TypeInfo> children;
    if (type.is_array) {
        std::size_t elements = 1;
        for (int dim : type.array_dims) {
            if (dim <= 0) return children;
            elements *= static_cast<std::size_t>(dim);
        }
        pred::v2::TypeInfo elem = scalarElementType(type);
        children.reserve(elements);
        for (std::size_t i = 0; i < elements; ++i) {
            children.push_back(elem);
        }
        return children;
    }
    if (const RecordMetadata* record = recordForType(context, type)) {
        children.reserve(record->fields.size());
        for (const RecordField& field : record->fields) {
            children.push_back(field.type);
        }
    }
    return children;
}

void appendExprLeaves(const ExprBuildContext& context,
                      const clang::Expr* expr,
                      std::optional<pred::v2::TypeInfo> expected_type,
                      std::vector<pred::v2::ExprPtr>& out,
                      std::vector<Diagnostic>& diagnostics);

void appendListLeaves(const ExprBuildContext& context,
                      const clang::InitListExpr* list,
                      std::optional<pred::v2::TypeInfo> expected_type,
                      std::vector<pred::v2::ExprPtr>& out,
                      std::vector<Diagnostic>& diagnostics) {
    if (!list) return;
    const clang::InitListExpr* semantic = list->isSemanticForm()
        ? list
        : (list->getSemanticForm() ? list->getSemanticForm() : list);
    std::vector<pred::v2::TypeInfo> child_types;
    if (expected_type) child_types = childTypesForAggregate(context, *expected_type);
    std::size_t index = 0;
    for (const clang::Expr* init : semantic->inits()) {
        std::optional<pred::v2::TypeInfo> child_type;
        if (index < child_types.size()) child_type = child_types[index];
        appendExprLeaves(context, init, std::move(child_type), out, diagnostics);
        ++index;
    }
}

void appendExprLeaves(const ExprBuildContext& context,
                      const clang::Expr* expr,
                      std::optional<pred::v2::TypeInfo> expected_type,
                      std::vector<pred::v2::ExprPtr>& out,
                      std::vector<Diagnostic>& diagnostics) {
    expr = unwrapTransparentExpr(expr);
    if (!expr) return;
    if (llvm::isa<clang::ImplicitValueInitExpr>(expr)) {
        if (expected_type) {
            appendDefaultLeaves(context, *expected_type, exprLoc(context, expr), out);
        }
        return;
    }
    if (const auto* construct = llvm::dyn_cast<clang::CXXConstructExpr>(expr)) {
        if (expected_type && construct->getNumArgs() == 0 &&
            (construct->requiresZeroInitialization() ||
             construct->isListInitialization())) {
            appendDefaultLeaves(context, *expected_type, exprLoc(context, construct), out);
            return;
        }
    }
    if (const auto* designated = llvm::dyn_cast<clang::DesignatedInitExpr>(expr)) {
        appendExprLeaves(context, designated->getInit(), std::move(expected_type),
                         out, diagnostics);
        return;
    }
    if (const auto* list = llvm::dyn_cast<clang::InitListExpr>(expr)) {
        appendListLeaves(context, list, std::move(expected_type), out, diagnostics);
        return;
    }
    auto built = buildExpr(context, expr);
    diagnostics.insert(diagnostics.end(),
                       built.diagnostics.begin(), built.diagnostics.end());
    if (built.expr) out.push_back(std::move(built.expr));
}

InitForm formFromDecl(const clang::VarDecl* decl,
                      const clang::Expr* init_expr,
                      bool aggregate,
                      bool designated) {
    if (designated) return InitForm::Designated;
    if (aggregate) return InitForm::Aggregate;
    if (!decl) {
        if (llvm::isa_and_nonnull<clang::InitListExpr>(unwrapTransparentExpr(init_expr))) {
            return InitForm::List;
        }
        return InitForm::Direct;
    }
    switch (decl->getInitStyle()) {
    case clang::VarDecl::CInit:
        return InitForm::Copy;
    case clang::VarDecl::CallInit:
        return InitForm::Direct;
    case clang::VarDecl::ListInit:
        return InitForm::List;
    }
    return InitForm::Copy;
}

bool isExplicitValueInitialization(const clang::VarDecl* decl,
                                   const clang::Expr* init_expr) {
    if (!init_expr) return false;
    init_expr = unwrapTransparentExpr(init_expr);
    if (const auto* list = llvm::dyn_cast_or_null<clang::InitListExpr>(init_expr)) {
        return decl && decl->getInitStyle() == clang::VarDecl::ListInit &&
               list->getNumInits() == 0;
    }
    if (const auto* construct =
            llvm::dyn_cast_or_null<clang::CXXConstructExpr>(init_expr)) {
        return construct->getNumArgs() == 0 &&
               (construct->requiresZeroInitialization() ||
                construct->isListInitialization());
    }
    return false;
}

bool sourceTextHasInitializer(const std::string& text,
                              const clang::VarDecl* decl) {
    if (!decl) return false;
    std::size_t name_pos = text.find(decl->getNameAsString());
    std::string tail = name_pos == std::string::npos
        ? text
        : text.substr(name_pos + decl->getNameAsString().size());
    return tail.find('=') != std::string::npos ||
           tail.find('{') != std::string::npos ||
           tail.find('(') != std::string::npos;
}

std::optional<std::string> sourceTextForDecl(const clang::VarDecl* decl) {
    if (!decl || decl->getSourceRange().isInvalid()) return std::nullopt;
    const clang::ASTContext& ast = decl->getASTContext();
    const clang::SourceManager& sm = ast.getSourceManager();
    bool invalid = false;
    llvm::StringRef text = clang::Lexer::getSourceText(
        clang::CharSourceRange::getTokenRange(decl->getSourceRange()),
        sm,
        ast.getLangOpts(),
        &invalid);
    if (invalid) return std::nullopt;
    return text.str();
}

bool hasSyntacticInitializerInSource(const ExprBuildContext& context,
                                     const clang::VarDecl* decl) {
    if (!decl) return false;
    std::optional<SourceTextSlice> source;
    if (context.session) {
        source = sourceTextForRange(*context.session, decl->getSourceRange());
    }
    if (source) return sourceTextHasInitializer(source->text, decl);
    std::optional<std::string> decl_source = sourceTextForDecl(decl);
    if (decl_source) return sourceTextHasInitializer(*decl_source, decl);
    return decl->hasInit();
}

} // namespace

bool hasSyntacticInitializer(const clang::VarDecl* decl) {
    if (!decl) return false;
    std::optional<std::string> decl_source = sourceTextForDecl(decl);
    if (decl_source) return sourceTextHasInitializer(*decl_source, decl);
    return decl->hasInit();
}

InitBuildResult buildAggregateInitializer(const ExprBuildContext& context,
                                          const InitBuildInput& input) {
    InitBuildResult result;
    const clang::Expr* init = input.init_expr;
    if (!init && input.decl) init = input.decl->getInit();

    result.form = containsDesignatedInit(init)
        ? InitForm::Designated
        : InitForm::Aggregate;

    appendExprLeaves(context, init, input.target_type, result.init_args,
                     result.diagnostics);
    if (hasError(result.diagnostics)) return result;

    std::size_t expected = leafCountForType(context, input.target_type);
    if (expected > 0) {
        std::vector<pred::v2::TypeInfo> leaf_types;
        appendLeafTypes(context, input.target_type, leaf_types);
        std::size_t current = leafCountForExprList(context, result.init_args);
        while (current < expected && current < leaf_types.size()) {
            result.init_args.push_back(
                defaultValueForScalar(leaf_types[current], input.loc));
            ++current;
        }
    }

    result.default_constructed = isExplicitValueInitialization(input.decl, init);
    return result;
}

InitBuildResult buildInitializer(const ExprBuildContext& context,
                                 const InitBuildInput& input) {
    InitBuildResult result;
    const clang::Expr* init = input.init_expr;
    if (!init && input.decl) init = input.decl->getInit();

    if (!init || (input.decl && !hasSyntacticInitializerInSource(context, input.decl))) {
        result.form = InitForm::None;
        result.default_constructed = false;
        return result;
    }

    const bool designated = containsDesignatedInit(init);
    const bool aggregate =
        input.target_type.is_array ||
        designated ||
        llvm::isa_and_nonnull<clang::InitListExpr>(unwrapTransparentExpr(init));

    if (aggregate) {
        return buildAggregateInitializer(context, input);
    }

    result.form = formFromDecl(input.decl, init, false, false);
    result.default_constructed = isExplicitValueInitialization(input.decl, init);

    auto built = buildExpr(context, init);
    result.diagnostics = std::move(built.diagnostics);
    if (!hasError(result.diagnostics) && built.expr) {
        result.init_expr = std::move(built.expr);
    }

    if (result.default_constructed && !result.init_expr.has_value()) {
        result.form = InitForm::Value;
        std::vector<pred::v2::ExprPtr> defaults;
        appendDefaultLeaves(context, input.target_type, input.loc, defaults);
        if (!defaults.empty()) result.init_expr = std::move(defaults.front());
    }
    return result;
}

} // namespace pred::s0clang18
