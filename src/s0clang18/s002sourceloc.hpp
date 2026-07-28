#pragma once

#include "s0clang18/s001session.hpp"

#include <clang/Basic/SourceLocation.h>

#include <optional>
#include <string>

namespace pred::s0clang18 {

enum class LocKind {
    Expansion,
    Spelling,
    Presumed,
};

struct SourceLocPolicy {
    LocKind primary_kind = LocKind::Expansion;
    bool preserve_macro_spelling_as_related = true;
    bool canonicalize_paths = true;
};

struct SourceTextSlice {
    std::string text;
    DebugLoc loc;
};

DebugLoc debugLocForLocation(const Clang18Session& session,
                             clang::SourceLocation loc,
                             const SourceLocPolicy& policy = {});

DebugLoc debugLocForRange(const Clang18Session& session,
                          clang::SourceRange range,
                          const SourceLocPolicy& policy = {});

std::optional<SourceTextSlice> sourceTextForRange(const Clang18Session& session,
                                                  clang::SourceRange range);

bool isInMainSourceFile(const Clang18Session& session, clang::SourceLocation loc);

} // namespace pred::s0clang18

