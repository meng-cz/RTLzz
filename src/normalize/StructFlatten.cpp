#include "normalize/NormalizeUtils.h"

#include <algorithm>
#include <cctype>

namespace pred {

std::string canonicalStructName(std::string name) {
    auto trim = [](std::string& s) {
        auto b = s.find_first_not_of(" \t\r\n");
        auto e = s.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) {
            s.clear();
        } else {
            s = s.substr(b, e - b + 1);
        }
    };
    auto comment = name.find("//");
    if (comment != std::string::npos) {
        name = name.substr(0, comment);
    }
    trim(name);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto prefix : {std::string("const "), std::string("volatile "), std::string("struct ")}) {
            if (name.rfind(prefix, 0) == 0) {
                name = name.substr(prefix.size());
                trim(name);
                changed = true;
            }
        }
    }
    while (!name.empty() && (name.back() == '&' || name.back() == '*')) {
        name.pop_back();
        trim(name);
    }
    return name;
}

} // namespace pred
