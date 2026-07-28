#pragma once

#include "debug/RTLZZException.h"

#include <clang/Basic/SourceLocation.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
class ASTUnit;
class ASTContext;
class CompilerInstance;
class SourceManager;
class TranslationUnitDecl;
} // namespace clang

namespace pred::s0clang18 {

enum class Severity {
    Note,
    Warning,
    Error,
};

struct Diagnostic {
    Severity severity = Severity::Error;
    ErrorContext context;
    std::string message;
};

template <typename T>
struct StepResult {
    std::optional<T> value;
    std::vector<Diagnostic> diagnostics;

    bool ok() const {
        for (const auto& diagnostic : diagnostics) {
            if (diagnostic.severity == Severity::Error) return false;
        }
        return value.has_value();
    }
};

struct Clang18Options {
    std::string source_name;
    std::optional<std::string> source_text;
    std::string top_function;
    std::vector<std::string> clang_args;
    std::string vullib_path;
    std::string cxx_standard = "c++20";
    bool debug_print = false;
};

struct Clang18Session {
    Clang18Session();
    Clang18Session(Clang18Session&&) noexcept;
    Clang18Session& operator=(Clang18Session&&) noexcept;
    Clang18Session(const Clang18Session&) = delete;
    Clang18Session& operator=(const Clang18Session&) = delete;
    ~Clang18Session();

    std::unique_ptr<clang::ASTUnit> ast_unit;
    std::unique_ptr<clang::CompilerInstance> compiler;
    clang::ASTContext* ast_context = nullptr;
    clang::SourceManager* source_manager = nullptr;
    clang::TranslationUnitDecl* translation_unit = nullptr;
    clang::FileID main_file_id;
    std::string main_file_path;
    std::vector<Diagnostic> diagnostics;
};

StepResult<Clang18Session> createClang18Session(const Clang18Options& options);

} // namespace pred::s0clang18
