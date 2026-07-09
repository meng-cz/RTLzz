#pragma once

#include "backend/beir.hpp"

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
    int max_iterations = 16;
};

Options parseOptions(const std::vector<std::string>& values);
Program optimizeProgram(Program program, const Options& options = Options{});

} // namespace pred::beir::opt
