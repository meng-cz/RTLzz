#pragma once

#include "debug/RTLZZException.h"
#include "s6inline/S6Inline.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pred::s7flatten {

using BlockId = s4cfg::BlockId;
using SymbolId = s3statementize::SymbolId;

enum class FlatOperandKind {
    Literal,
    Var,
    Unknown,
};

struct FlatOperand {
    FlatOperandKind kind = FlatOperandKind::Unknown;
    TypeInfo type;
    DebugLoc debug_loc;
    std::string literal_value;
    std::string var_name;
    SymbolId var_symbol = -1;
};

struct FlatOpExpr {
    enum class Kind {
        Unary,
        Binary,
        Ternary,
        Cast,
        Hardware,
    };

    Kind kind = Kind::Unary;
    TypeInfo type;
    DebugLoc debug_loc;
    s3statementize::UnaryOp unary_op = s3statementize::UnaryOp::Plus;
    s3statementize::BinaryOp binary_op = s3statementize::BinaryOp::Add;
    s3statementize::HardwareOp hardware_op = s3statementize::HardwareOp::Concat;
    TypeInfo cast_type;
    std::vector<FlatOperand> operands;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    int to_width = 0;
};

enum class FlatStmtKind {
    Decl,
    Assign,
    Op,
    Lookup,
    LookupWrite,
    Eval,
};

struct FlattenedStmt {
    FlatStmtKind kind = FlatStmtKind::Eval;
    DebugLoc debug_loc;

    TypeInfo decl_type;
    std::string decl_name;
    SymbolId decl_symbol = -1;

    SymbolId target_symbol = -1;
    std::string target_name;
    TypeInfo target_type;
    FlatOperand value;
    FlatOpExpr op;

    FlatOperand lookup_index;
    std::vector<FlatOperand> lookup_elements;

    // `lookupwrite` updates a full scalar leaf array in one statement:
    // targets[k] receives the array after assigning `lookup_value` at
    // `lookup_index`. The old array values are `lookup_elements`.
    std::vector<SymbolId> lookup_write_target_symbols;
    std::vector<std::string> lookup_write_target_names;
    FlatOperand lookup_value;
};

using FlattenedStmtPtr = std::shared_ptr<FlattenedStmt>;

struct FlattenedEdge {
    BlockId from = -1;
    BlockId to = -1;
    s4cfg::EdgeKind kind = s4cfg::EdgeKind::Jump;
    std::string label;
    std::optional<FlatOperand> case_value;
};

struct FlattenedSwitchTarget {
    std::optional<FlatOperand> value;
    BlockId target = -1;
};

struct FlattenedTerminator {
    s4cfg::TermKind kind = s4cfg::TermKind::Unreachable;
    FlatOperand condition;
    BlockId jump_target = -1;
    BlockId true_target = -1;
    BlockId false_target = -1;
    FlatOperand switch_value;
    std::vector<FlattenedSwitchTarget> switch_targets;
    BlockId default_target = -1;
    std::optional<FlatOperand> return_value;
};

struct FlattenedBasicBlock {
    BlockId id = -1;
    std::vector<FlattenedStmtPtr> stmts;
    FlattenedTerminator terminator;
    std::vector<FlattenedEdge> successors;
    std::vector<FlattenedEdge> predecessors;
};

struct LeafInfo {
    SymbolId id = -1;
    std::string name;
    TypeInfo type;
    std::vector<std::string> path;
    DebugLoc debug_loc;
};

struct SymbolLeafMap {
    SymbolId source_symbol = -1;
    std::string source_name;
    TypeInfo source_type;
    std::vector<LeafInfo> leaves;
};

struct FlattenedPort {
    std::string source_name;
    TypeInfo source_type;
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
    std::vector<LeafInfo> leaves;
};

struct FlattenedFunction {
    std::string name;
    TypeInfo return_type;
    std::vector<ParamDecl> params;
    std::vector<s3statementize::SymbolInfo> symbols;
    std::vector<FlattenedPort> ports;
    std::vector<SymbolLeafMap> symbol_leaf_maps;
    BlockId entry = -1;
    BlockId exit = -1;
    std::vector<std::unique_ptr<FlattenedBasicBlock>> blocks;
};

struct FlattenedProgram {
    FlattenedFunction top;
    std::unordered_map<std::string, std::vector<StructFieldInfo>> struct_fields;
    std::unordered_map<std::string, std::vector<StructConstructorInfo>> struct_constructors;
};

struct FlattenWarning {
    ErrorContext context;
    std::string message;
};

struct FlattenError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct FlattenOptions {
    bool debug_print = false;
    int max_leaf_symbols = 4096;
};

struct FlattenSummary {
    std::string function_name;
    int aggregate_symbols = 0;
    int leaf_symbols = 0;
    int dynamic_reads = 0;
    int dynamic_writes = 0;
};

struct FlattenResult {
    std::optional<FlattenedProgram> program;
    std::optional<FlattenError> error;
    std::vector<FlattenWarning> warnings;
    std::vector<FlattenSummary> summaries;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

FlattenResult flattenProgram(
    const s6inline::InlinedCFGProgram& program,
    const FlattenOptions& options = {});

FlattenedProgram flattenProgramOrThrow(
    const s6inline::InlinedCFGProgram& program,
    const FlattenOptions& options = {});

std::string debugPrint(const FlattenedProgram& program,
                       const std::vector<FlattenSummary>& summaries);

} // namespace pred::s7flatten
