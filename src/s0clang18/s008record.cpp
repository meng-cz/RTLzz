#include "s0clang18/s008record.hpp"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const Clang18Session& session,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.8";
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

const clang::CXXRecordDecl* asCXXRecordDefinition(const clang::RecordDecl* decl) {
    if (!decl) return nullptr;
    const clang::RecordDecl* definition = decl->getDefinition();
    if (!definition) return nullptr;
    return llvm::dyn_cast<clang::CXXRecordDecl>(definition);
}

class RecordDeclCollector
    : public clang::RecursiveASTVisitor<RecordDeclCollector> {
public:
    explicit RecordDeclCollector(const Clang18Session& session)
        : session_(session) {}

    bool VisitRecordDecl(clang::RecordDecl* decl) {
        const clang::CXXRecordDecl* definition = asCXXRecordDefinition(decl);
        if (!definition || !definition->isCompleteDefinition()) return true;
        if (!isInMainSourceFile(session_, definition->getLocation())) return true;
        const auto* canonical =
            llvm::dyn_cast<clang::RecordDecl>(definition->getCanonicalDecl());
        records.insert(canonical ? canonical : definition);
        return true;
    }

    std::unordered_set<const clang::RecordDecl*> records;

private:
    const Clang18Session& session_;
};

class ParamRefFinder
    : public clang::RecursiveASTVisitor<ParamRefFinder> {
public:
    explicit ParamRefFinder(
        const std::unordered_map<const clang::ParmVarDecl*, std::string>& params)
        : params_(params) {}

    bool VisitDeclRefExpr(clang::DeclRefExpr* expr) {
        const auto* param = llvm::dyn_cast<clang::ParmVarDecl>(expr->getDecl());
        if (param && params_.count(param)) found = param;
        return found == nullptr;
    }

    const clang::ParmVarDecl* found = nullptr;

private:
    const std::unordered_map<const clang::ParmVarDecl*, std::string>& params_;
};

std::string paramName(const clang::ParmVarDecl* param, int index) {
    if (!param) return {};
    std::string name = param->getNameAsString();
    if (!name.empty()) return name;
    return "arg" + std::to_string(index);
}

RecordConstructor collectConstructor(const Clang18Session& session,
                                     const clang::CXXConstructorDecl* ctor,
                                     const SourceLocPolicy& loc_policy) {
    RecordConstructor out;
    out.constructor_decl = ctor;
    out.loc = debugLocForRange(session, ctor->getSourceRange(), loc_policy);

    std::unordered_map<const clang::ParmVarDecl*, std::string> params;
    int index = 0;
    for (const clang::ParmVarDecl* param : ctor->parameters()) {
        std::string name = paramName(param, index++);
        out.param_names.push_back(name);
        params.emplace(param, std::move(name));
    }

    for (const clang::CXXCtorInitializer* init : ctor->inits()) {
        if (!init || !init->isAnyMemberInitializer()) continue;
        const clang::FieldDecl* field = init->getAnyMember();
        if (!field || !init->getInit()) continue;
        ParamRefFinder finder(params);
        finder.TraverseStmt(init->getInit());
        if (!finder.found) continue;
        auto param_name = params.find(finder.found);
        if (param_name == params.end()) continue;
        out.field_to_param[field->getNameAsString()] =
            param_name->second;
    }
    return out;
}

StepResult<RecordMetadata> collectOneRecord(
    const Clang18Session& session,
    const clang::RecordDecl* canonical_decl,
    const TypeLoweringContext& type_context,
    const SourceLocPolicy& loc_policy) {
    StepResult<RecordMetadata> result;
    const clang::CXXRecordDecl* record = asCXXRecordDefinition(canonical_decl);
    if (!record) {
        result.diagnostics.push_back(makeError(
            session, {}, "Record metadata requires a complete CXXRecordDecl"));
        return result;
    }

    clang::QualType record_type = session.ast_context->getRecordType(
        const_cast<clang::CXXRecordDecl*>(record));
    auto key_result = canonicalRecordKey(type_context, record_type,
                                         debugLocForRange(session, record->getSourceRange(), loc_policy));
    if (!key_result.ok()) {
        result.diagnostics = std::move(key_result.diagnostics);
        return result;
    }

    RecordMetadata metadata;
    metadata.key = *key_result.value;
    metadata.aggregate_initializable = record->isAggregate();
    metadata.loc = debugLocForRange(session, record->getSourceRange(), loc_policy);

    for (const clang::FieldDecl* field : record->fields()) {
        DebugLoc field_loc = debugLocForRange(session, field->getSourceRange(), loc_policy);
        auto lowered = lowerQualType(type_context, field->getType(), field_loc);
        if (!lowered.ok()) {
            result.diagnostics.insert(result.diagnostics.end(),
                                      lowered.diagnostics.begin(),
                                      lowered.diagnostics.end());
            continue;
        }
        if (lowered.value->type.is_reference || lowered.value->type.is_pointer) {
            result.diagnostics.push_back(makeError(
                session, field_loc,
                "Record field '" + field->getNameAsString() +
                    "' must not be a reference or pointer"));
            continue;
        }

        RecordField out_field;
        out_field.name = field->getNameAsString();
        out_field.type = lowered.value->type;
        out_field.field_decl = field;
        out_field.loc = std::move(field_loc);
        metadata.fields.push_back(std::move(out_field));
    }

    for (const clang::CXXConstructorDecl* ctor : record->ctors()) {
        if (!ctor || ctor->isImplicit()) continue;
        RecordConstructor out_ctor = collectConstructor(session, ctor, loc_policy);
        if (!out_ctor.param_names.empty()) {
            metadata.constructors.push_back(std::move(out_ctor));
        }
    }

    if (hasError(result.diagnostics)) return result;
    result.value = std::move(metadata);
    return result;
}

void addRecordToSet(RecordMetadataSet& set, RecordMetadata metadata) {
    std::size_t index = set.records.size();
    if (metadata.key.canonical_decl) {
        set.record_by_decl[metadata.key.canonical_decl] = index;
    }
    if (!metadata.key.canonical_name.empty()) {
        set.record_by_name[metadata.key.canonical_name] = index;
    }
    set.records.push_back(std::move(metadata));
}

} // namespace

StepResult<RecordMetadataSet> collectRecordMetadata(
    const Clang18Session& session,
    const TopFunctionSelection& top,
    const PortDeclTable& ports,
    const TypeLoweringContext& type_context,
    const SourceLocPolicy& loc_policy) {
    StepResult<RecordMetadataSet> result;
    if (!session.ast_context || !session.translation_unit) {
        result.diagnostics.push_back(makeError(
            session, {}, "S0Clang18 record metadata requires a valid Clang18Session"));
        return result;
    }
    if (!top.function_decl) {
        result.diagnostics.push_back(makeError(
            session, top.loc, "Record metadata requires a selected top function"));
        return result;
    }

    RecordDeclCollector collector(session);
    collector.TraverseDecl(session.translation_unit);

    for (const auto& port : ports.ports) {
        if (!port.type.struct_name.empty()) {
            auto key = canonicalRecordKey(type_context, port.var_decl->getType(), port.decl_loc);
            if (key.ok() && key.value && key.value->canonical_decl) {
                collector.records.insert(key.value->canonical_decl);
            }
        }
    }

    RecordMetadataSet set;
    for (const clang::RecordDecl* record : collector.records) {
        auto metadata = collectOneRecord(session, record, type_context, loc_policy);
        if (!metadata.ok()) {
            result.diagnostics.insert(result.diagnostics.end(),
                                      metadata.diagnostics.begin(),
                                      metadata.diagnostics.end());
            continue;
        }
        addRecordToSet(set, std::move(*metadata.value));
    }

    if (hasError(result.diagnostics)) return result;
    result.value = std::move(set);
    return result;
}

const RecordMetadata* findRecordMetadata(const RecordMetadataSet& records,
                                         const RecordTypeKey& key) {
    if (key.canonical_decl) {
        auto by_decl = records.record_by_decl.find(key.canonical_decl);
        if (by_decl != records.record_by_decl.end() &&
            by_decl->second < records.records.size()) {
            return &records.records[by_decl->second];
        }
    }
    if (!key.canonical_name.empty()) {
        auto by_name = records.record_by_name.find(key.canonical_name);
        if (by_name != records.record_by_name.end() &&
            by_name->second < records.records.size()) {
            return &records.records[by_name->second];
        }
    }
    return nullptr;
}

} // namespace pred::s0clang18
