#pragma once

#include "debug/RTLZZException.h"
#include "s4cfg/S4CFG.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s6inline {

using BlockId = s4cfg::BlockId;

struct InlinedBasicBlock {
    BlockId id = -1;
    std::vector<s4cfg::CFGStmt> stmts;
    s4cfg::Terminator terminator;
    std::vector<s4cfg::CFGEdge> successors;
    std::vector<s4cfg::CFGEdge> predecessors;
};

struct InlinedFunction {
    std::string name;
    TypeInfo return_type;
    std::vector<ParamDecl> params;
    // SymbolId remains function-local unique. S6 introduces fresh ids for every
    // cloned callee local/value parameter/return slot and does not preserve
    // lexical scope metadata as semantics.
    std::vector<s3statementize::SymbolInfo> symbols;
    BlockId entry = -1;
    BlockId exit = -1;
    std::vector<std::unique_ptr<InlinedBasicBlock>> blocks;
    std::optional<std::string> return_slot;
    s3statementize::SymbolId return_slot_symbol = -1;
};

struct InlinedCFGProgram {
    InlinedFunction top;
    std::unordered_map<std::string, std::vector<StructFieldInfo>> struct_fields;
    std::unordered_map<std::string, std::vector<StructConstructorInfo>> struct_constructors;
};

struct InlineWarning {
    ErrorContext context;
    std::string message;
};

struct InlineError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct InlineOptions {
    bool debug_print = false;
    int max_inline_depth = 128;
    int max_cloned_blocks = 100000;
};

struct InlineSummary {
    std::string caller;
    std::string callee;
    BlockId call_block = -1;
    int cloned_blocks = 0;
};

struct InlineResult {
    std::optional<InlinedCFGProgram> program;
    std::optional<InlineError> error;
    std::vector<InlineWarning> warnings;
    std::vector<InlineSummary> summaries;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

InlineResult inlineCFGProgram(
    const s4cfg::CFGProgram& program,
    const InlineOptions& options = {});

InlinedCFGProgram inlineCFGProgramOrThrow(
    const s4cfg::CFGProgram& program,
    const InlineOptions& options = {});

std::string debugPrint(const InlinedCFGProgram& program,
                       const std::vector<InlineSummary>& summaries);

} // namespace pred::s6inline
