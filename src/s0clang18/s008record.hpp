#pragma once

#include "s0clang18/s007consteval.hpp"
#include "v2/V2Types.h"

#include <clang/AST/DeclCXX.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s0clang18 {

struct RecordField {
    std::string name;
    pred::v2::TypeInfo type;
    const clang::FieldDecl* field_decl = nullptr;
    DebugLoc loc;
};

struct RecordConstructor {
    const clang::CXXConstructorDecl* constructor_decl = nullptr;
    std::vector<std::string> param_names;
    std::unordered_map<std::string, std::string> field_to_param;
    DebugLoc loc;
};

struct RecordMetadata {
    RecordTypeKey key;
    std::vector<RecordField> fields;
    std::vector<RecordConstructor> constructors;
    bool aggregate_initializable = false;
    DebugLoc loc;
};

struct RecordMetadataSet {
    std::vector<RecordMetadata> records;
    std::unordered_map<const clang::RecordDecl*, std::size_t> record_by_decl;
    std::unordered_map<std::string, std::size_t> record_by_name;
};

StepResult<RecordMetadataSet> collectRecordMetadata(
    const Clang18Session& session,
    const TopFunctionSelection& top,
    const PortDeclTable& ports,
    const TypeLoweringContext& type_context,
    const SourceLocPolicy& loc_policy = {});

const RecordMetadata* findRecordMetadata(const RecordMetadataSet& records,
                                         const RecordTypeKey& key);

} // namespace pred::s0clang18

