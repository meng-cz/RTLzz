#pragma once

#include "debug/RTLZZException.h"
#include "v2/V2AST.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s0ast {

using EntityId = int;

enum class S0Substage {
    ClangSessionAndRawCursor,
    V2ASTDataModel,
    TypeAndEntityCollect,
    ExprStmtBuild,
    ResolveAndSurfaceValidate,
    PipelineBridge,
};

struct S0Diagnostic {
    ErrorContext context;
    std::string message;
};

struct S0ParseOptions {
    bool debug_print = false;
};

struct S0Type {
    pred::v2::TypeInfo type;
    bool signed_view = false;
};

struct S0Param {
    EntityId id = -1;
    std::string name;
    S0Type type;
    DebugLoc debug_loc;
    pred::v2::ParamDirection direction = pred::v2::ParamDirection::Input;
    pred::v2::ParamPassingKind passing = pred::v2::ParamPassingKind::Value;
};

enum class S0ExprKind {
    Literal,
    VarRef,
    Unary,
    Binary,
    Ternary,
    Call,
    Cast,
    ArrayAccess,
    FieldAccess,
    HardwareSurface,
};

struct S0Expr;
using S0ExprPtr = std::shared_ptr<S0Expr>;

struct S0Expr {
    EntityId id = -1;
    S0ExprKind kind = S0ExprKind::Literal;
    S0Type type;
    DebugLoc debug_loc;

    std::string text;
    std::string name;
    std::string op;
    std::vector<S0ExprPtr> operands;
    std::vector<int> template_args;
    std::optional<EntityId> resolved_callee;
};

enum class S0StmtKind {
    Decl,
    Assign,
    If,
    For,
    While,
    DoWhile,
    Switch,
    Block,
    Break,
    Continue,
    Return,
    ExprStmt,
};

struct S0Stmt;
using S0StmtPtr = std::shared_ptr<S0Stmt>;

struct S0Stmt {
    EntityId id = -1;
    S0StmtKind kind = S0StmtKind::ExprStmt;
    DebugLoc debug_loc;
    std::string name;
    S0Type type;
    std::vector<S0ExprPtr> exprs;
    std::vector<S0StmtPtr> children;
};

enum class S0FunctionKind {
    Top,
    Helper,
    Lambda,
};

struct S0Function {
    EntityId id = -1;
    std::string name;
    S0FunctionKind kind = S0FunctionKind::Helper;
    S0Type return_type;
    std::vector<S0Param> params;
    std::vector<S0StmtPtr> body;
    std::vector<EntityId> nested_lambdas;
    DebugLoc debug_loc;
};

struct S0Program {
    std::string source_name;
    EntityId top_function = -1;
    std::vector<S0Function> functions;
    std::unordered_map<std::string, std::vector<pred::v2::StructFieldInfo>> struct_fields;
    std::unordered_map<std::string, std::vector<pred::v2::StructConstructorInfo>> struct_constructors;

    pred::v2::FunctionAST surface_ast;
};

struct S0Result {
    std::optional<S0Program> program;
    std::optional<S0Diagnostic> error;
    std::vector<S0Diagnostic> warnings;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

S0Result parseProgram(const std::string& source_name,
                      const std::optional<std::string>& source_text,
                      const std::string& top_function,
                      const std::vector<std::string>& clang_args,
                      const S0ParseOptions& options = {});

const pred::v2::FunctionAST& surfaceAST(const S0Program& program);

std::string debugPrint(const S0Program& program);

const char* substageName(S0Substage substage);

} // namespace pred::s0ast
