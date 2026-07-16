#pragma once

#include "debug/RTLZZException.h"
#include "s8opnorm/S8Norm.h"

#include <optional>
#include <string>
#include <vector>

namespace pred::s9ssa {

using BlockId = int;
using SymbolId = int;
using S9ValueId = int;

using S9Type = s8opnorm::S8Type;
using S9Literal = s8opnorm::S8Literal;
using S9OpKind = s8opnorm::S8OpKind;

enum class S9ValueKind {
    Initial,
    Statement,
    Phi,
    Generated,
};

struct S9Value {
    S9ValueId id = -1;
    SymbolId base_symbol = -1;
    int version = -1;
    S9Type type;
    S9ValueKind kind = S9ValueKind::Statement;
    std::string debug_name;
    BlockId def_block = -1;
};

enum class S9OperandKind {
    Literal,
    Value,
};

struct S9Operand {
    S9OperandKind kind = S9OperandKind::Literal;
    S9Type type;
    bool signed_view = false;
    DebugLoc debug_loc;
    S9Literal literal;
    S9ValueId value = -1;
};

struct S9Operation {
    S9OpKind kind = S9OpKind::AssignCast;
    DebugLoc debug_loc;
    std::vector<S9Operand> operands;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    int result_width = 0;
};

enum class S9StmtKind {
    Assign,
    Op,
    Lookup,
};

struct S9Stmt {
    S9StmtKind kind = S9StmtKind::Assign;
    DebugLoc debug_loc;
    S9ValueId target = -1;
    S9Operand value;
    S9Operation op;
    S9Operand lookup_index;
    std::vector<S9Operand> lookup_elements;
    std::string debug_note;
};

struct S9PhiIncoming {
    BlockId pred = -1;
    S9ValueId value = -1;
};

struct S9Phi {
    S9ValueId result = -1;
    SymbolId base_symbol = -1;
    DebugLoc debug_loc;
    std::vector<S9PhiIncoming> incoming;
};

struct S9SwitchTarget {
    std::optional<S9Operand> value;
    BlockId target = -1;
};

using S9TermKind = s8opnorm::S8TermKind;

struct S9Terminator {
    S9TermKind kind = S9TermKind::Unreachable;
    S9Operand condition;
    BlockId jump_target = -1;
    BlockId true_target = -1;
    BlockId false_target = -1;
    S9Operand switch_value;
    std::vector<S9SwitchTarget> switch_targets;
    BlockId default_target = -1;
};

struct S9BasicBlock {
    BlockId id = -1;
    bool reachable = false;
    std::vector<S9Phi> phis;
    std::vector<S9Stmt> stmts;
    S9Terminator terminator;
};

using S9SymbolRole = s8opnorm::S8SymbolRole;

struct S9Symbol {
    SymbolId id = -1;
    S9Type type;
    std::string debug_name;
    S9SymbolRole role = S9SymbolRole::Local;
};

struct S9Port {
    SymbolId symbol = -1;
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
    std::optional<S9ValueId> initial_value;
    std::optional<S9ValueId> final_value;
};

struct S9PortElement {
    SymbolId symbol = -1;
    std::vector<int> indices;
};

struct S9PortGroup {
    std::string source_name;
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
    TypeInfo source_type;
    TypeInfo scalar_source_type;
    S9Type scalar_type;
    std::vector<int> array_dims;
    std::vector<S9PortElement> elements;
};

struct S9SSACFG {
    std::string name;
    std::vector<S9Symbol> base_symbols;
    std::vector<S9Port> ports;
    std::vector<S9PortGroup> port_groups;
    std::vector<S9Value> values;
    BlockId entry = -1;
    BlockId exit = -1;
    std::vector<S9BasicBlock> blocks;
};

struct S9SSAProgram {
    S9SSACFG top;
};

struct SSABuildWarning {
    ErrorContext context;
    std::string message;
};

struct SSABuildError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct SSAOptions {
    bool debug_print = false;
    int max_values = 200000;
};

struct SSASummary {
    std::string function_name;
    int values = 0;
    int phis = 0;
    int lowered_lookupwrites = 0;
    int generated_ops = 0;
    int reachable_blocks = 0;
};

struct SSAResult {
    std::optional<S9SSAProgram> program;
    std::optional<SSABuildError> error;
    std::vector<SSABuildWarning> warnings;
    std::vector<SSASummary> summaries;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

SSAResult buildSSA(const s8opnorm::S8NormProgram& program,
                   const SSAOptions& options = {});

S9SSAProgram buildSSAOrThrow(const s8opnorm::S8NormProgram& program,
                             const SSAOptions& options = {});

void verifySSAProgram(const S9SSAProgram& program);

std::string debugPrint(const S9SSAProgram& program,
                       const std::vector<SSASummary>& summaries);

} // namespace pred::s9ssa
