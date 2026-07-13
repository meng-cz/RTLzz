#include "backend/beopt.hpp"

#include "backend/beopt_algebraic.hpp"
#include "backend/beopt_assign_chains.hpp"
#include "backend/beopt_constant.hpp"
#include "backend/beopt_cse.hpp"
#include "backend/beopt_dce.hpp"
#include "backend/beopt_predicate.hpp"
#include "backend/beopt_width.hpp"

#include <stdexcept>
#include <utility>

namespace pred::beir::opt {

Options parseOptions(const std::vector<std::string>& values) {
    Options options;
    for (const std::string& value : values) {
        if (value == "all") {
            options.fold_assign_chains = true;
            options.constant_folding = true;
            options.width_simplification = true;
            options.algebraic_identities = true;
            options.common_subexpressions = true;
            options.dead_node_elimination = true;
            options.predicate_sinking = true;
        } else if (value == "none") {
            options.fold_assign_chains = false;
            options.constant_folding = false;
            options.width_simplification = false;
            options.algebraic_identities = false;
            options.common_subexpressions = false;
            options.dead_node_elimination = false;
            options.predicate_sinking = false;
        } else if (value == "assign" || value == "fold-assign") {
            options.fold_assign_chains = true;
        } else if (value == "no-assign" || value == "no-fold-assign") {
            options.fold_assign_chains = false;
        } else if (value == "cse") {
            options.common_subexpressions = true;
        } else if (value == "no-cse") {
            options.common_subexpressions = false;
        } else if (value == "constant" || value == "const" || value == "constant-fold") {
            options.constant_folding = true;
        } else if (value == "no-constant" || value == "no-const" || value == "no-constant-fold") {
            options.constant_folding = false;
        } else if (value == "bitvalue" || value == "bit-value") {
            options.constant_folding = true;
        } else if (value == "no-bitvalue" || value == "no-bit-value") {
            options.constant_folding = false;
        } else if (value == "width" || value == "width-simplify") {
            options.width_simplification = true;
        } else if (value == "no-width" || value == "no-width-simplify") {
            options.width_simplification = false;
        } else if (value == "algebraic" || value == "arith") {
            options.algebraic_identities = true;
        } else if (value == "no-algebraic" || value == "no-arith") {
            options.algebraic_identities = false;
        } else if (value == "predicate" || value == "predicate-sinking") {
            options.predicate_sinking = true;
        } else if (value == "no-predicate" || value == "no-predicate-sinking") {
            options.predicate_sinking = false;
        } else if (value == "dce") {
            options.dead_node_elimination = true;
        } else if (value == "no-dce") {
            options.dead_node_elimination = false;
        } else {
            throw std::runtime_error("unknown BEIR optimization option: " + value);
        }
    }
    return options;
}

Program optimizeProgram(Program program, const Options& options) {
    MutableProgram graph(std::move(program));
    bool changed = true;
    int iteration = 0;
    while (changed && iteration++ < options.max_iterations) {
        changed = false;
        if (options.fold_assign_chains) changed = foldAssignChains(graph) || changed;
        if (options.constant_folding) changed = foldConstants(graph) || changed;
        if (options.algebraic_identities) changed = simplifyAlgebraicIdentities(graph) || changed;
        if (options.width_simplification) changed = simplifyWidthOperations(graph) || changed;
        if (options.common_subexpressions) changed = mergeCommonExpressions(graph) || changed;
        if (options.fold_assign_chains) changed = foldAssignChains(graph) || changed;
        if (options.dead_node_elimination) changed = eliminateDeadNodes(graph) || changed;
    }
    if (options.predicate_sinking && sinkPredicates(graph)) {
        if (options.fold_assign_chains) foldAssignChains(graph);
        if (options.common_subexpressions) mergeCommonExpressions(graph);
        if (options.fold_assign_chains) foldAssignChains(graph);
        if (options.dead_node_elimination) eliminateDeadNodes(graph);
    }
    return graph.finish();
}

} // namespace pred::beir::opt
