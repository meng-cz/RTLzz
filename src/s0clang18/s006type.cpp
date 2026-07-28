#include "s0clang18/s006type.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Type.h>
#include <llvm/Support/Casting.h>

#include <algorithm>
#include <limits>
#include <sstream>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const TypeLoweringContext& context,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.6";
    if (context.session) diagnostic.context.source_file = context.session->main_file_path;
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

clang::ASTContext* astContext(const TypeLoweringContext& context) {
    return context.session ? context.session->ast_context : nullptr;
}

bool hasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

std::optional<int> integralTemplateArg(const clang::TemplateArgument& arg) {
    if (arg.getKind() != clang::TemplateArgument::Integral) return std::nullopt;
    long long value = arg.getAsIntegral().getExtValue();
    if (value <= 0 || value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::string canonicalRecordName(const clang::RecordDecl* decl) {
    if (!decl) return {};
    const clang::RecordDecl* canonical =
        llvm::dyn_cast<clang::RecordDecl>(decl->getCanonicalDecl());
    if (!canonical) canonical = decl;
    std::string name = canonical->getQualifiedNameAsString();
    if (!name.empty()) return name;
    name = decl->getQualifiedNameAsString();
    if (!name.empty()) return name;
    return decl->getNameAsString();
}

const clang::RecordDecl* canonicalRecordDecl(const clang::RecordDecl* decl) {
    if (!decl) return nullptr;
    const auto* canonical =
        llvm::dyn_cast<clang::RecordDecl>(decl->getCanonicalDecl());
    return canonical ? canonical : decl;
}

std::optional<RecordTypeKey> recordKeyForType(clang::QualType type) {
    if (type.isNull()) return std::nullopt;
    clang::QualType unqualified = type.getNonReferenceType().getUnqualifiedType();
    const auto* record_type = unqualified->getAs<clang::RecordType>();
    if (!record_type) {
        record_type = unqualified.getCanonicalType()->getAs<clang::RecordType>();
    }
    if (!record_type) return std::nullopt;
    const clang::RecordDecl* canonical = canonicalRecordDecl(record_type->getDecl());
    if (!canonical) return std::nullopt;
    return RecordTypeKey{canonical, canonicalRecordName(canonical)};
}

std::optional<const clang::ClassTemplateSpecializationDecl*>
classTemplateSpecialization(clang::QualType type) {
    if (type.isNull()) return std::nullopt;
    clang::QualType unqualified = type.getNonReferenceType().getUnqualifiedType();
    const auto* record_type = unqualified->getAs<clang::RecordType>();
    if (!record_type) {
        record_type = unqualified.getCanonicalType()->getAs<clang::RecordType>();
    }
    if (!record_type) return std::nullopt;
    const auto* specialization =
        llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record_type->getDecl());
    if (!specialization) return std::nullopt;
    return specialization;
}

StepResult<LoweredType> failType(const TypeLoweringContext& context,
                                 DebugLoc loc,
                                 std::string message) {
    StepResult<LoweredType> result;
    result.diagnostics.push_back(makeError(context, std::move(loc), std::move(message)));
    return result;
}

StepResult<RecordTypeKey> failRecordKey(const TypeLoweringContext& context,
                                        DebugLoc loc,
                                        std::string message) {
    StepResult<RecordTypeKey> result;
    result.diagnostics.push_back(makeError(context, std::move(loc), std::move(message)));
    return result;
}

StepResult<LoweredType> lowerNonCvRefType(const TypeLoweringContext& context,
                                          clang::QualType type,
                                          DebugLoc loc);

StepResult<LoweredType> lowerStdArrayType(
    const TypeLoweringContext& context,
    clang::QualType original_type,
    const clang::ClassTemplateSpecializationDecl* specialization,
    DebugLoc loc) {
    const clang::TemplateArgumentList& args = specialization->getTemplateArgs();
    if (args.size() < 2 ||
        args[0].getKind() != clang::TemplateArgument::Type) {
        return failType(context, loc, "std::array type has unsupported template arguments");
    }
    std::optional<int> size = integralTemplateArg(args[1]);
    if (!size) {
        return failType(context, loc, "std::array size is not a supported positive integer");
    }

    StepResult<LoweredType> elem =
        lowerQualType(context, args[0].getAsType(), loc);
    if (!elem.ok()) return elem;

    LoweredType out = *elem.value;
    out.type.name = original_type.getUnqualifiedType().getAsString();
    out.type.is_array = true;
    out.type.array_size = *size;
    out.type.array_dims.insert(out.type.array_dims.begin(), *size);
    out.record_key.reset();
    StepResult<LoweredType> result;
    result.value = std::move(out);
    return result;
}

StepResult<LoweredType> lowerIntTemplateType(
    const TypeLoweringContext& context,
    const clang::ClassTemplateSpecializationDecl* specialization,
    DebugLoc loc) {
    const clang::TemplateArgumentList& args = specialization->getTemplateArgs();
    if (args.size() < 1) {
        return failType(context, loc, "Int<N> type has no width template argument");
    }
    std::optional<int> width = integralTemplateArg(args[0]);
    if (!width) {
        return failType(context, loc, "Int<N> width is not a supported positive integer");
    }

    LoweredType lowered;
    lowered.type = pred::v2::make_hw_type("Int", *width, false);
    StepResult<LoweredType> result;
    result.value = std::move(lowered);
    return result;
}

StepResult<LoweredType> lowerBuiltinType(const TypeLoweringContext& context,
                                         clang::QualType type,
                                         DebugLoc loc) {
    clang::ASTContext* ast = astContext(context);
    if (!ast) return failType(context, loc, "Type lowering requires ASTContext");

    if (type->isBooleanType()) {
        LoweredType lowered;
        lowered.type = pred::v2::make_bool_type();
        StepResult<LoweredType> result;
        result.value = std::move(lowered);
        return result;
    }
    if (!type->isIntegerType() || type->isEnumeralType()) {
        return failType(context, loc, "Type is not a supported builtin integer");
    }

    int width = static_cast<int>(ast->getTypeSize(type));
    if (width <= 0) return failType(context, loc, "Builtin integer has invalid width");
    if (context.options.reject_int128_builtin && width > 64) {
        return failType(context, loc, "Builtin 128-bit integers are not supported; use Int<N>");
    }

    LoweredType lowered;
    lowered.type.name = type.getUnqualifiedType().getAsString();
    lowered.type.width = width;
    lowered.type.is_signed = type->isSignedIntegerType();
    lowered.type.is_hw_int = true;
    lowered.type.hw_kind = "builtin";
    StepResult<LoweredType> result;
    result.value = std::move(lowered);
    return result;
}

StepResult<LoweredType> lowerEnumType(const TypeLoweringContext& context,
                                      clang::QualType type,
                                      DebugLoc loc) {
    clang::ASTContext* ast = astContext(context);
    if (!ast) return failType(context, loc, "Type lowering requires ASTContext");

    const auto* enum_type = type->getAs<clang::EnumType>();
    if (!enum_type || !enum_type->getDecl()) {
        return failType(context, loc, "Enum type has no declaration");
    }
    clang::QualType underlying = enum_type->getDecl()->getIntegerType();
    if (underlying.isNull()) {
        return failType(context, loc, "Enum type has no supported underlying type");
    }

    int width = static_cast<int>(ast->getTypeSize(underlying));
    if (width <= 0) return failType(context, loc, "Enum underlying type has invalid width");

    LoweredType lowered;
    lowered.type.name = type.getUnqualifiedType().getAsString();
    lowered.type.width = width;
    lowered.type.is_signed = underlying->isSignedIntegerType();
    lowered.type.is_hw_int = true;
    lowered.type.hw_kind = "enum";
    StepResult<LoweredType> result;
    result.value = std::move(lowered);
    return result;
}

StepResult<LoweredType> lowerRecordType(const TypeLoweringContext& context,
                                        clang::QualType type,
                                        DebugLoc loc) {
    std::optional<RecordTypeKey> key = recordKeyForType(type);
    if (!key) return failType(context, loc, "Record type has no canonical record declaration");

    LoweredType lowered;
    lowered.record_key = *key;
    lowered.type.name = key->canonical_name;
    lowered.type.struct_name = key->canonical_name;
    StepResult<LoweredType> result;
    result.value = std::move(lowered);
    return result;
}

StepResult<LoweredType> lowerConstantArrayType(const TypeLoweringContext& context,
                                               clang::QualType type,
                                               DebugLoc loc) {
    clang::ASTContext* ast = astContext(context);
    if (!ast) return failType(context, loc, "Type lowering requires ASTContext");
    const clang::ConstantArrayType* array_type = ast->getAsConstantArrayType(type);
    if (!array_type) return failType(context, loc, "Type is not a constant array");

    llvm::APInt size_ap = array_type->getSize();
    if (size_ap.getActiveBits() > 31) {
        return failType(context, loc, "Constant array size is too large");
    }
    int size = static_cast<int>(size_ap.getZExtValue());
    if (size <= 0) return failType(context, loc, "Constant array size must be positive");

    StepResult<LoweredType> elem =
        lowerQualType(context, array_type->getElementType(), loc);
    if (!elem.ok()) return elem;

    LoweredType out = *elem.value;
    out.type.name = type.getUnqualifiedType().getAsString();
    out.type.is_array = true;
    out.type.array_size = size;
    out.type.array_dims.insert(out.type.array_dims.begin(), size);
    out.record_key.reset();
    StepResult<LoweredType> result;
    result.value = std::move(out);
    return result;
}

StepResult<LoweredType> lowerNonCvRefType(const TypeLoweringContext& context,
                                          clang::QualType type,
                                          DebugLoc loc) {
    if (type.isNull()) return failType(context, loc, "Cannot lower null QualType");
    if (type.isVolatileQualified()) {
        return failType(context, loc, "volatile-qualified types are not supported");
    }

    if (type->isConstantArrayType()) {
        return lowerConstantArrayType(context, type, loc);
    }

    if (auto specialization = classTemplateSpecialization(type)) {
        std::string qualified_name =
            (*specialization)->getSpecializedTemplate()->getQualifiedNameAsString();
        std::string template_name =
            (*specialization)->getSpecializedTemplate()->getNameAsString();
        if (template_name == "Int" &&
            (qualified_name == "Int" || qualified_name == "vulfixint::Int")) {
            return lowerIntTemplateType(context, *specialization, loc);
        }
        if (qualified_name == "std::array") {
            return lowerStdArrayType(context, type, *specialization, loc);
        }
    }

    if (type->isEnumeralType()) {
        return lowerEnumType(context, type, loc);
    }

    if (type->isBooleanType() || type->isIntegerType()) {
        return lowerBuiltinType(context, type, loc);
    }

    if (recordKeyForType(type)) {
        return lowerRecordType(context, type, loc);
    }

    return failType(context, loc, "Unsupported type: " + type.getAsString());
}

} // namespace

StepResult<LoweredType> lowerQualType(const TypeLoweringContext& context,
                                      clang::QualType type,
                                      DebugLoc loc) {
    if (!context.session || !context.session->ast_context) {
        return failType(context, loc, "Type lowering requires a valid Clang18Session");
    }
    if (type.isNull()) return failType(context, loc, "Cannot lower null QualType");
    if (type.isVolatileQualified()) {
        return failType(context, loc, "volatile-qualified types are not supported");
    }

    bool top_const = type.isConstQualified();
    if (const auto* reference = type->getAs<clang::ReferenceType>()) {
        if (!context.options.allow_reference_types) {
            return failType(context, loc, "Reference types are not allowed in this context");
        }
        clang::QualType pointee = reference->getPointeeType();
        StepResult<LoweredType> lowered =
            lowerQualType(context, pointee.getUnqualifiedType(), loc);
        if (!lowered.ok()) return lowered;
        lowered.value->type.is_reference = true;
        lowered.value->type.is_const = pointee.isConstQualified();
        lowered.value->type.is_mutable = !lowered.value->type.is_const;
        return lowered;
    }

    if (const auto* pointer = type->getAs<clang::PointerType>()) {
        if (!context.options.allow_pointer_types) {
            return failType(context, loc, "Pointer types are not allowed in this context");
        }
        clang::QualType pointee = pointer->getPointeeType();
        StepResult<LoweredType> lowered =
            lowerQualType(context, pointee.getUnqualifiedType(), loc);
        if (!lowered.ok()) return lowered;
        lowered.value->type.is_pointer = true;
        lowered.value->type.is_const = pointee.isConstQualified();
        lowered.value->type.is_mutable = !lowered.value->type.is_const;
        return lowered;
    }

    StepResult<LoweredType> lowered =
        lowerNonCvRefType(context, type.getUnqualifiedType(), loc);
    if (!lowered.ok()) return lowered;
    lowered.value->type.is_const = top_const;
    lowered.value->type.is_mutable = !top_const;
    return lowered;
}

StepResult<RecordTypeKey> canonicalRecordKey(const TypeLoweringContext& context,
                                             clang::QualType type,
                                             DebugLoc loc) {
    if (!context.session || !context.session->ast_context) {
        return failRecordKey(context, loc, "Record key lowering requires a valid Clang18Session");
    }
    std::optional<RecordTypeKey> key = recordKeyForType(type);
    if (!key) {
        return failRecordKey(context, loc, "Type is not a record type: " + type.getAsString());
    }
    StepResult<RecordTypeKey> result;
    result.value = *key;
    return result;
}

std::string typeLabel(const pred::v2::TypeInfo& type) {
    std::string base;
    if (!type.struct_name.empty()) {
        base = type.struct_name;
    } else if (!type.name.empty()) {
        base = type.name;
    } else {
        base = "<unknown>";
    }
    if (type.is_array) {
        for (int dim : type.array_dims) {
            base += "[" + std::to_string(dim) + "]";
        }
    }
    if (type.is_pointer) base += "*";
    if (type.is_reference) base += "&";
    if (type.is_const) base = "const " + base;
    return base;
}

} // namespace pred::s0clang18
