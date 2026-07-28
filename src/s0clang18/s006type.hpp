#pragma once

#include "s0clang18/s005semantic.hpp"
#include "v2/V2Types.h"

#include <clang/AST/Type.h>

#include <optional>
#include <string>

namespace pred::s0clang18 {

struct TypeLoweringOptions {
    bool allow_pointer_types = false;
    bool allow_reference_types = true;
    bool reject_int128_builtin = true;
};

struct TypeLoweringContext {
    const Clang18Session* session = nullptr;
    const SemanticIndex* semantic_index = nullptr;
    TypeLoweringOptions options;
};

struct RecordTypeKey {
    const clang::RecordDecl* canonical_decl = nullptr;
    std::string canonical_name;
};

struct LoweredType {
    pred::v2::TypeInfo type;
    std::optional<RecordTypeKey> record_key;
};

StepResult<LoweredType> lowerQualType(const TypeLoweringContext& context,
                                      clang::QualType type,
                                      DebugLoc loc = {});

StepResult<RecordTypeKey> canonicalRecordKey(const TypeLoweringContext& context,
                                             clang::QualType type,
                                             DebugLoc loc = {});

std::string typeLabel(const pred::v2::TypeInfo& type);

} // namespace pred::s0clang18

