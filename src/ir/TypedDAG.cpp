#include "ir/TypedDAG.h"

#include <functional>
#include <sstream>

namespace pred {
namespace {

std::string jsonEscape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

std::string IRType::key() const {
    return kind + ":" + std::to_string(width) + ":" + (is_signed ? "s" : "u");
}

IRValue TypedDAG::makeNode(std::string kind,
                           IRType type,
                           std::vector<IRValue> operands,
                           std::map<std::string, std::string> attrs,
                           IRSourceLoc loc) {
    std::vector<int> operand_ids;
    operand_ids.reserve(operands.size());
    for (auto operand : operands) operand_ids.push_back(operand.node_id);
    std::string key = keyFor(kind, type, operand_ids, attrs);
    auto it = node_by_key_.find(key);
    if (it != node_by_key_.end()) return IRValue{it->second};

    int id = static_cast<int>(nodes_.size());
    IRNode n;
    n.id = id;
    n.kind = std::move(kind);
    n.type = std::move(type);
    n.operands = std::move(operand_ids);
    n.attrs = std::move(attrs);
    n.loc = std::move(loc);
    nodes_.push_back(std::move(n));
    node_by_key_[key] = id;
    return IRValue{id};
}

IRValue TypedDAG::makeVar(const std::string& name, IRType type) {
    return makeNode("var", std::move(type), {}, {{"name", name}});
}

IRValue TypedDAG::makeLiteral(const std::string& value, IRType type) {
    return makeNode("literal", std::move(type), {}, {{"value", value}});
}

bool TypedDAG::hasCycle() const {
    std::vector<int> state(nodes_.size(), 0);
    std::function<bool(int)> dfs = [&](int id) {
        if (id < 0 || id >= static_cast<int>(nodes_.size())) return false;
        if (state[id] == 1) return true;
        if (state[id] == 2) return false;
        state[id] = 1;
        for (int child : nodes_[id].operands) {
            if (dfs(child)) return true;
        }
        state[id] = 2;
        return false;
    };
    for (const auto& n : nodes_) {
        if (dfs(n.id)) return true;
    }
    return false;
}

std::string TypedDAG::stableJson() const {
    std::ostringstream os;
    os << "{\"nodes\":[";
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const auto& n = nodes_[i];
        if (i) os << ",";
        os << "{\"id\":" << n.id
           << ",\"kind\":\"" << jsonEscape(n.kind) << "\""
           << ",\"type\":{\"kind\":\"" << jsonEscape(n.type.kind)
           << "\",\"width\":" << n.type.width
           << ",\"signed\":" << (n.type.is_signed ? "true" : "false") << "}"
           << ",\"operands\":[";
        for (size_t j = 0; j < n.operands.size(); ++j) {
            if (j) os << ",";
            os << n.operands[j];
        }
        os << "],\"attrs\":{";
        size_t attr_index = 0;
        for (const auto& [key, value] : n.attrs) {
            if (attr_index++) os << ",";
            os << "\"" << jsonEscape(key) << "\":\"" << jsonEscape(value) << "\"";
        }
        os << "}}";
    }
    os << "],\"assignments\":[],\"outputs\":[],\"assumptions\":[],\"diagnostics\":[]}";
    return os.str();
}

std::string TypedDAG::keyFor(const std::string& kind,
                             const IRType& type,
                             const std::vector<int>& operands,
                             const std::map<std::string, std::string>& attrs) const {
    std::ostringstream os;
    os << kind << "|" << type.key() << "|";
    for (int operand : operands) os << operand << ",";
    os << "|";
    for (const auto& [key, value] : attrs) os << key << "=" << value << ";";
    return os.str();
}

} // namespace pred
