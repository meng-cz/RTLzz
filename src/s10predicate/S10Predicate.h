#pragma once

#include "debug/RTLZZException.h"
#include "s9ssa/S9SSA.h"

#include <optional>
#include <string>
#include <vector>

namespace pred::s10predicate {

using BlockId = int;
using SymbolId = int;
using S10ValueId = int;

using S10Type = s9ssa::S9Type;
using S10Literal = s9ssa::S9Literal;
using S10OpKind = s9ssa::S9OpKind;

enum class S10ValueKind {
    Initial,
    Statement,
    Phi,
    Generated,
};

struct S10Value {
    S10ValueId id = -1;
    SymbolId base_symbol = -1;
    int version = -1;
    S10Type type;
    S10ValueKind kind = S10ValueKind::Statement;
    std::string debug_name;
    BlockId source_block = -1;
};

enum class S10OperandKind {
    Literal,
    Value,
};

struct S10Operand {
    S10OperandKind kind = S10OperandKind::Literal;
    S10Type type;
    bool signed_view = false;
    DebugLoc debug_loc;
    S10Literal literal;
    S10ValueId value = -1;
};

struct S10Operation {
    S10OpKind kind = S10OpKind::AssignCast;
    DebugLoc debug_loc;
    std::vector<S10Operand> operands;
    int hi = -1;
    int lo = -1;
    int bit = -1;
    int times = 0;
    int result_width = 0;
};

enum class S10DefKind {
    Assign,
    Op,
    Lookup,
};

struct S10Definition {
    S10DefKind kind = S10DefKind::Assign;
    DebugLoc debug_loc;
    S10Operand guard;
    S10ValueId target = -1;
    S10Operand value;
    S10Operation op;
    S10Operand lookup_index;
    std::vector<S10Operand> lookup_elements;
    BlockId source_block = -1;
    std::string debug_note;
};

struct S10BlockGuard {
    BlockId block = -1;
    bool reachable = false;
    std::optional<S10Operand> guard;
};

using S10SymbolRole = s9ssa::S9SymbolRole;

struct S10Symbol {
    SymbolId id = -1;
    S10Type type;
    std::string debug_name;
    S10SymbolRole role = S10SymbolRole::Local;
};

struct S10Port {
    SymbolId symbol = -1;
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
    std::optional<S10ValueId> initial_value;
    std::optional<S10ValueId> final_value;
    std::optional<S10Operand> final_guard;
};

struct S10PortElement {
    SymbolId symbol = -1;
    std::vector<int> indices;
};

struct S10PortGroup {
    std::string source_name;
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
    TypeInfo source_type;
    TypeInfo scalar_source_type;
    S10Type scalar_type;
    std::vector<int> array_dims;
    std::vector<S10PortElement> elements;
};

struct S10PredicateProgram {
    std::string name;
    std::vector<S10Symbol> base_symbols;
    std::vector<S10Value> values;
    std::vector<S10Port> ports;
    std::vector<S10PortGroup> port_groups;
    std::vector<S10BlockGuard> block_guards;
    std::vector<S10Definition> definitions;
};

struct PredicateWarning {
    ErrorContext context;
    std::string message;
};

struct PredicateError {
    ErrorContext context;
    std::string message;
    std::string formatted;
};

struct PredicateOptions {
    bool debug_print = false;
    int max_values = 300000;
};

struct PredicateSummary {
    std::string function_name;
    int values = 0;
    int definitions = 0;
    int block_guards = 0;
    int lowered_phis = 0;
    int generated_guards = 0;
    int ignored_unreachable_blocks = 0;
};

struct PredicateResult {
    std::optional<S10PredicateProgram> program;
    std::optional<PredicateError> error;
    std::vector<PredicateWarning> warnings;
    std::vector<PredicateSummary> summaries;
    std::string debug_text;

    bool ok() const { return !error.has_value(); }
};

PredicateResult lowerPredicates(
    const s9ssa::S9SSAProgram& program,
    const PredicateOptions& options = {});

S10PredicateProgram lowerPredicatesOrThrow(
    const s9ssa::S9SSAProgram& program,
    const PredicateOptions& options = {});

void verifyPredicateProgram(const S10PredicateProgram& program);

std::string debugPrint(const S10PredicateProgram& program,
                       const std::vector<PredicateSummary>& summaries);

} // namespace pred::s10predicate
