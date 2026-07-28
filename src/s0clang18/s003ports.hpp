#pragma once

#include "s0clang18/s002sourceloc.hpp"
#include "v2/V2Types.h"

#include <clang/AST/Decl.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s0clang18 {

enum class PortDirection {
    Input,
    Output,
};

struct PortPragma {
    std::string name;
    PortDirection direction = PortDirection::Input;
    DebugLoc loc;
};

struct RawPortDecl {
    std::string name;
    PortDirection direction = PortDirection::Input;
    const clang::VarDecl* var_decl = nullptr;
    pred::v2::TypeInfo type;
    DebugLoc decl_loc;
    DebugLoc pragma_loc;
};

struct PortDeclTable {
    std::vector<PortPragma> pragmas;
    std::vector<RawPortDecl> ports;
    std::unordered_map<std::string, std::size_t> port_by_name;
};

StepResult<PortDeclTable> collectPragmaAndPortDecls(
    const Clang18Session& session,
    const SourceLocPolicy& loc_policy = {});

pred::v2::ParamDirection toV2Direction(PortDirection direction);

} // namespace pred::s0clang18

