#pragma once

#include <string>
#include <vector>

namespace pred {

struct IRLValue {
    std::string root;
    std::vector<std::string> fields;
    std::vector<int> indices;
};

} // namespace pred
