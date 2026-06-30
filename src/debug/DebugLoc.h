#pragma once

#include <string>

namespace pred {

struct DebugLoc {
    std::string file;
    int line = 0;
    int column = 0;
    int end_line = 0;
    int end_column = 0;

    bool valid() const {
        return !file.empty() || line > 0 || column > 0 ||
               end_line > 0 || end_column > 0;
    }
};

} // namespace pred
