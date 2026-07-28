#include "s0clang18/s002sourceloc.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

[[noreturn]] static void failCheck(const char* expr, const char* file, int line) {
    std::cerr << file << ":" << line << ": CHECK failed: " << expr << "\n";
    std::exit(1);
}

#define CHECK(condition) \
    do { \
        if (!(condition)) failCheck(#condition, __FILE__, __LINE__); \
    } while (false)

namespace {

class FindDeclVisitor : public clang::RecursiveASTVisitor<FindDeclVisitor> {
public:
    bool VisitFunctionDecl(clang::FunctionDecl* decl) {
        if (decl->getIdentifier() && decl->getName() == "hls_main") {
            hls_main = decl;
        }
        return true;
    }

    bool VisitVarDecl(clang::VarDecl* decl) {
        if (!decl->getIdentifier()) return true;
        if (decl->getName() == "plain_value") plain_value = decl;
        if (decl->getName() == "from_macro") from_macro = decl;
        return true;
    }

    clang::FunctionDecl* hls_main = nullptr;
    clang::VarDecl* plain_value = nullptr;
    clang::VarDecl* from_macro = nullptr;
};

pred::s0clang18::Clang18Session buildSession() {
    pred::s0clang18::Clang18Options options;
    options.source_name = "testv2/clang18/s002_virtual_input.logic.cpp";
    options.source_text = R"cpp(#define MAKE_VALUE 17
void hls_main() {
  int plain_value = 5 + 6;
  int from_macro = MAKE_VALUE;
}
#line 200 "presumed_user.logic.cpp"
void presumed_func() {}
)cpp";
    options.top_function = "hls_main";
    options.clang_args = {"-I.", "-Ithird_party/vulsim/vullib"};

    auto result = pred::s0clang18::createClang18Session(options);
    if (!result.ok()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.message << "\n";
        }
    }
    CHECK(result.ok());
    CHECK(result.value.has_value());
    return std::move(*result.value);
}

FindDeclVisitor collectDecls(pred::s0clang18::Clang18Session& session) {
    FindDeclVisitor visitor;
    visitor.TraverseDecl(session.ast_context->getTranslationUnitDecl());
    CHECK(visitor.hls_main != nullptr);
    CHECK(visitor.plain_value != nullptr);
    CHECK(visitor.from_macro != nullptr);
    return visitor;
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void testPlainRangeAndSourceText() {
    auto session = buildSession();
    auto decls = collectDecls(session);

    pred::s0clang18::SourceLocPolicy policy;
    policy.canonicalize_paths = false;
    pred::DebugLoc loc =
        pred::s0clang18::debugLocForRange(session, decls.plain_value->getSourceRange(), policy);

    CHECK(endsWith(loc.file, "s002_virtual_input.logic.cpp"));
    CHECK(loc.line == 3);
    CHECK(loc.column > 0);
    CHECK(loc.end_line == 3);
    CHECK(loc.end_column > loc.column);

    auto text = pred::s0clang18::sourceTextForRange(
        session, decls.plain_value->getSourceRange());
    CHECK(text.has_value());
    CHECK(text->text.find("plain_value = 5 + 6") != std::string::npos);
    CHECK(text->loc.valid());
}

void testMacroExpansionAndSpellingLocs() {
    auto session = buildSession();
    auto decls = collectDecls(session);
    CHECK(decls.from_macro->getInit() != nullptr);

    pred::s0clang18::SourceLocPolicy expansion;
    expansion.primary_kind = pred::s0clang18::LocKind::Expansion;
    expansion.canonicalize_paths = false;
    pred::DebugLoc expansion_loc = pred::s0clang18::debugLocForLocation(
        session, decls.from_macro->getInit()->getBeginLoc(), expansion);
    CHECK(expansion_loc.line == 4);

    pred::s0clang18::SourceLocPolicy spelling;
    spelling.primary_kind = pred::s0clang18::LocKind::Spelling;
    spelling.canonicalize_paths = false;
    pred::DebugLoc spelling_loc = pred::s0clang18::debugLocForLocation(
        session, decls.from_macro->getInit()->getBeginLoc(), spelling);
    CHECK(spelling_loc.line == 1);
}

void testPresumedLocAndMainFile() {
    auto session = buildSession();
    auto decls = collectDecls(session);

    pred::s0clang18::SourceLocPolicy policy;
    policy.primary_kind = pred::s0clang18::LocKind::Presumed;
    policy.canonicalize_paths = false;
    pred::DebugLoc presumed =
        pred::s0clang18::debugLocForLocation(session, decls.hls_main->getLocation(), policy);
    CHECK(endsWith(presumed.file, "s002_virtual_input.logic.cpp"));

    CHECK(pred::s0clang18::isInMainSourceFile(session, decls.hls_main->getLocation()));
}

} // namespace

int main() {
    testPlainRangeAndSourceText();
    testMacroExpansionAndSpellingLocs();
    testPresumedLocAndMainFile();
    std::cout << "s002sourceloc_test passed\n";
    return 0;
}
