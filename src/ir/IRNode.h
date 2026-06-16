#pragma once

#include "ir/IRType.h"

#include <map>
#include <string>
#include <vector>

namespace pred {

struct IRSourceLoc {
    std::string file;
    int line = 0;
    int column = 0;
};

struct IRNode {
    int id = -1;
    std::string kind;
    IRType type;
    std::vector<int> operands;
    std::map<std::string, std::string> attrs;
    IRSourceLoc loc;
};

} // namespace pred
