#include "backend/beopt.hpp"

#include "backend/beopt_algebraic.hpp"
#include "backend/beopt_assign_chains.hpp"
#include "backend/beopt_constant.hpp"
#include "backend/beopt_cse.hpp"
#include "backend/beopt_dce.hpp"
#include "backend/beopt_predicate.hpp"
#include "backend/beopt_width.hpp"
#include "backend/beopt_slices.hpp"
#include "backend/beopt_structure.hpp"
#include "backend/beopt_case_guards.hpp"
#include "backend/beopt_bit_updates.hpp"
#include "backend/beopt_boolean.hpp"

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
            options.exclusive_muxes = true;
            options.balance_trees = true;
            options.bit_range_update_coalescing = true;
            options.boolean_control_normalization = true;
        } else if (value == "none") {
            options.fold_assign_chains = false;
            options.constant_folding = false;
            options.width_simplification = false;
            options.algebraic_identities = false;
            options.common_subexpressions = false;
            options.dead_node_elimination = false;
            options.predicate_sinking = false;
            options.exclusive_muxes = false;
            options.balance_trees = false;
            options.bit_range_update_coalescing = false;
            options.boolean_control_normalization = false;
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
        } else if (value == "mux") {
            options.exclusive_muxes = true;
        } else if (value == "no-mux") {
            options.exclusive_muxes = false;
        } else if (value == "balance") {
            options.balance_trees = true;
        } else if (value == "no-balance") {
            options.balance_trees = false;
        } else if (value == "bit-updates" || value == "coalesce-bit-updates") {
            options.bit_range_update_coalescing = true;
        } else if (value == "no-bit-updates" || value == "no-coalesce-bit-updates") {
            options.bit_range_update_coalescing = false;
        } else if (value == "boolean" || value == "boolean-control") {
            options.boolean_control_normalization = true;
        } else if (value == "no-boolean" || value == "no-boolean-control") {
            options.boolean_control_normalization = false;
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

Program optimizeProgram(Program program,
                        const Options& options,
                        const IterationCallback& iteration_callback) {
    MutableProgram graph(std::move(program));
    const int iter_before_predicate_sinking = options.max_iterations > 8 ? 4 : std::max(1, options.max_iterations / 2);
    for (int iteration = 1; iteration <= options.max_iterations; ++iteration) {
        const std::string iter_msg = "iteration " + std::to_string(iteration) + "/" + std::to_string(options.max_iterations);
        const auto run_pass = [&](const char* name, auto pass, auto&&... args) {
            if (iteration_callback) iteration_callback(iter_msg + ": " + name);
            pass(graph, std::forward<decltype(args)>(args)...);
        };
        if (options.fold_assign_chains) run_pass("foldAssignChains", foldAssignChains);
        if (options.constant_folding) run_pass("foldConstants", foldConstants);
        if (options.width_simplification) run_pass("specializeConstantSlices", specializeConstantSlices);
        if (options.bit_range_update_coalescing) {
            run_pass("coalesceBitRangeUpdates", coalesceBitRangeUpdates,
                     options.max_bit_range_updates, options.max_bit_compose_pieces);
        }
        if (options.algebraic_identities) run_pass("simplifyAlgebraicIdentities", simplifyAlgebraicIdentities);
        if (options.width_simplification) run_pass("simplifyWidthOperations", simplifyWidthOperations);
        if (options.common_subexpressions) run_pass("mergeCommonExpressions", mergeCommonExpressions);
        if (options.fold_assign_chains) run_pass("foldAssignChains", foldAssignChains);
        if (options.dead_node_elimination) run_pass("eliminateDeadNodes", eliminateDeadNodes);
        if (iteration == iter_before_predicate_sinking) {
            if (options.predicate_sinking) {
                if (options.boolean_control_normalization)
                    run_pass("normalizeBooleanControl", normalizeBooleanControl);
                if (iteration_callback) iteration_callback(iter_msg + ": sinkPredicates");
                sinkPredicates(graph, {options.max_predicate_formulas, options.max_predicate_atoms});
                if (options.boolean_control_normalization)
                    run_pass("normalizeBooleanControl", normalizeBooleanControl);
            }
            if (options.exclusive_muxes) {
                run_pass("parallelizeExclusiveMuxes", parallelizeExclusiveMuxes, options.max_mux_branches);
                run_pass("simplifyCaseGuards", [](MutableProgram& graph) { simplifyCaseGuards(graph); });
            }
            if (options.balance_trees) {
                run_pass("balanceAssociativeTrees", balanceAssociativeTrees, options.max_tree_leaves);
            }
        }
        if (iteration == options.max_iterations && options.boolean_control_normalization)
            run_pass("normalizeBooleanControl", normalizeBooleanControl);
    }
    return graph.finish();
}

} // namespace pred::beir::opt
