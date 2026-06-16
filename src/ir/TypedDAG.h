#pragma once

#include "ir/IRNode.h"
#include "ir/IRValue.h"

#include <map>
#include <string>
#include <vector>

namespace pred {

class TypedDAG {
public:
    IRValue makeNode(std::string kind,
                     IRType type,
                     std::vector<IRValue> operands = {},
                     std::map<std::string, std::string> attrs = {},
                     IRSourceLoc loc = {});
    IRValue makeVar(const std::string& name, IRType type);
    IRValue makeLiteral(const std::string& value, IRType type);

    const std::vector<IRNode>& nodes() const { return nodes_; }
    bool hasCycle() const;
    std::string stableJson() const;

private:
    std::string keyFor(const std::string& kind,
                       const IRType& type,
                       const std::vector<int>& operands,
                       const std::map<std::string, std::string>& attrs) const;

    std::vector<IRNode> nodes_;
    std::map<std::string, int> node_by_key_;
};

} // namespace pred
