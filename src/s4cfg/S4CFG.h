#pragma once

#include "debug/RTLZZException.h"
#include "s3statementize/S3Statementize.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s4cfg {

using TypeInfo = pred::v2::TypeInfo;
using ParamDecl = pred::v2::ParamDecl;
using StructFieldInfo = pred::v2::StructFieldInfo;
using StructConstructorInfo = pred::v2::StructConstructorInfo;

using BlockId = int;
using LoopRegionId = int;

enum class CFGStmtKind {
    Decl,
    Assign,
    Op,
    Call,
    Construct,
    Eval,
};

enum class EdgeKind {
    Fallthrough,
    Jump,
    True,
    False,
    Case,
    Default,
    Break,
    Continue,
    Return,
};

enum class TermKind {
    Jump,
    Branch,
    Switch,
    Return,
    Unreachable,
    Exit,
};

enum class LoopConditionKind {
    PreTest,
    PostTest,
};

struct CFGStmt {
    CFGStmtKind kind = CFGStmtKind::Eval;
    s3statementize::S3StmtPtr stmt;
};

struct CFGEdge {
    BlockId from = -1;
    BlockId to = -1;
    EdgeKind kind = EdgeKind::Jump;
    std::string label;
    std::optional<s3statementize::Operand> case_value;
};

struct SwitchTarget {
    std::optional<s3statementize::Operand> value;
    BlockId target = -1;
};

struct Terminator {
    TermKind kind = TermKind::Unreachable;
    s3statementize::Operand condition;
    BlockId jump_target = -1;
    BlockId true_target = -1;
    BlockId false_target = -1;
    s3statementize::Operand switch_value;
    std::vector<SwitchTarget> switch_targets;
    BlockId default_target = -1;
    std::optional<s3statementize::Operand> return_value;
};

struct BasicBlock {
    BlockId id = -1;
    std::vector<CFGStmt> stmts;
    Terminator terminator;
    std::vector<CFGEdge> successors;
    std::vector<CFGEdge> predecessors;
    std::vector<LoopRegionId> loop_stack;
};

struct LoopRegion {
    LoopRegionId id = -1;
    LoopConditionKind condition_kind = LoopConditionKind::PreTest;
    BlockId init = -1;
    BlockId condition = -1;
    BlockId condition_prelude = -1;
    BlockId body = -1;
    BlockId exit = -1;
};

struct FunctionCFG {
    std::string name;
    TypeInfo return_type;
    std::vector<ParamDecl> params;
    // SymbolId remains function-local unique after S3. S4/S5 and later stages
    // must use these ids as variable identity and must not depend on lexical
    // scope metadata for name resolution.
    std::vector<s3statementize::SymbolInfo> symbols;
    BlockId entry = -1;
    BlockId exit = -1;
    std::vector<std::unique_ptr<BasicBlock>> blocks;
    std::vector<LoopRegion> loop_regions;
    std::optional<std::string> return_slot;
    s3statementize::SymbolId return_slot_symbol = -1;
};

struct CFGProgram {
    FunctionCFG top;
    std::vector<FunctionCFG> helpers;
    std::unordered_map<std::string, FunctionCFG> lambdas;
    std::unordered_map<std::string, std::vector<StructFieldInfo>> struct_fields;
    std::unordered_map<std::string, std::vector<StructConstructorInfo>> struct_constructors;
    std::unordered_map<std::string, std::size_t> helper_index;
    std::unordered_map<std::string, std::string> lambda_index;
};

struct CFGWarning {
    ErrorContext context;
    std::string message;
};

struct CFGError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct CFGOptions {
    bool debug_print = false;
};

struct CFGResult {
    std::optional<CFGProgram> program;
    std::optional<CFGError> error;
    std::vector<CFGWarning> warnings;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

CFGResult buildCFGProgram(
    const s3statementize::StatementizedProgram& program,
    const CFGOptions& options = {});

CFGProgram buildCFGProgramOrThrow(
    const s3statementize::StatementizedProgram& program,
    const CFGOptions& options = {});

std::string debugPrint(const CFGProgram& program);

void lowerFunctionExits(FunctionCFG& cfg, std::vector<CFGWarning>& warnings);

} // namespace pred::s4cfg
