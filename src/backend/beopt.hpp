#pragma once

#include "backend/beir.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pred::beir::opt {

struct Options {
    bool fold_assign_chains = true;
    bool common_subexpressions = true;
    bool dead_node_elimination = true;
    bool constant_folding = true;
    bool width_simplification = true;
    bool algebraic_identities = true;
    bool predicate_sinking = true;
    int max_iterations = 8;
    bool exclusive_muxes = true;
    bool balance_trees = true;
    bool bit_range_update_coalescing = true;
    bool boolean_control_normalization = true;
    // Predicate sinking participates in the main optimizer fixed point, while
    // this bound prevents it from dominating large graphs indefinitely.
    int max_predicate_iterations = 4;
    std::size_t max_predicate_formulas = 16384;
    std::size_t max_predicate_atoms = 512;
    unsigned max_mux_branches = 1024;
    unsigned max_tree_leaves = 1024;
    unsigned max_bit_range_updates = 1024;
    unsigned max_bit_compose_pieces = 2048;
};

Options parseOptions(const std::vector<std::string>& values);
using IterationCallback = std::function<void(const std::string& iteration)>;

Program optimizeProgram(Program program,
                        const Options& options = Options{},
                        const IterationCallback& iteration_callback = {});

} // namespace pred::beir::opt
