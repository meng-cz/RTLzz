#pragma once

#include "s0clang18/s004top.hpp"

#include <clang/AST/Decl.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s0clang18 {

using SemanticEntityId = int;

enum class SemanticEntityKind {
    Unknown,
    Variable,
    Field,
    Function,
    FunctionTemplate,
    Method,
    LambdaCallOperator,
    Record,
};

struct SemanticEntity {
    SemanticEntityId id = -1;
    SemanticEntityKind kind = SemanticEntityKind::Unknown;
    std::string name;
    const clang::Decl* decl = nullptr;
    DebugLoc loc;
};

struct SemanticIndex {
    std::vector<SemanticEntity> entities;
    std::unordered_map<const clang::Decl*, SemanticEntityId> entity_by_decl;
    std::unordered_map<std::string, std::vector<SemanticEntityId>> entities_by_name;
};

StepResult<SemanticIndex> buildSemanticIndex(
    const Clang18Session& session,
    const TopFunctionSelection& top,
    const PortDeclTable& ports,
    const SourceLocPolicy& loc_policy = {});

const SemanticEntity* findEntity(const SemanticIndex& index, const clang::Decl* decl);

} // namespace pred::s0clang18

