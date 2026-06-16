#pragma once

#include <string>

namespace pred {

struct IRType {
    std::string kind = "bits";
    int width = 0;
    bool is_signed = false;

    std::string key() const;
};

} // namespace pred
