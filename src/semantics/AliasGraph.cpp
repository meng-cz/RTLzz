#include "semantics/AliasGraph.h"

namespace pred {
namespace {

std::string joinAliasPath(const std::string& object,
                          const std::vector<std::string>& fields,
                          const std::vector<int>& indices) {
    std::string out = object;
    for (const auto& field : fields) {
        if (!out.empty()) out += ".";
        out += field;
    }
    for (int index : indices) {
        out += "[";
        out += std::to_string(index);
        out += "]";
    }
    return out;
}

AliasTarget completeTarget(std::string alias, AliasTarget target) {
    if (target.canonical_name.empty()) {
        target.canonical_name = target.name.empty() ? std::move(alias) : target.name;
    }
    if (target.base_object.empty()) target.base_object = target.canonical_name;
    if (target.mutability == AliasMutability::Unknown) {
        target.mutability = target.writable ? AliasMutability::Mutable : AliasMutability::Immutable;
    }
    return target;
}

} // namespace

void AliasGraph::bind(const std::string& alias, AliasTarget target) {
    if (!alias.empty()) aliases_[alias] = completeTarget(alias, std::move(target));
}

void AliasGraph::bindReference(const std::string& alias, const std::string& target, bool is_const,
                               std::string reason) {
    AliasTarget t;
    t.name = target;
    t.canonical_name = target;
    t.base_object = target;
    t.kind = is_const ? AliasKind::ConstRef : AliasKind::MutableRef;
    t.mutability = is_const ? AliasMutability::Immutable : AliasMutability::Mutable;
    t.writable = !is_const;
    t.reason = std::move(reason);
    bind(alias, std::move(t));
}

void AliasGraph::bindPointer(const std::string& alias, const std::string& target,
                             std::string reason) {
    AliasTarget t;
    t.name = target;
    t.canonical_name = target;
    t.base_object = target;
    t.kind = AliasKind::Pointer;
    t.mutability = AliasMutability::Mutable;
    t.writable = true;
    t.reason = std::move(reason);
    bind(alias, std::move(t));
}

void AliasGraph::bindField(const std::string& object, const std::string& field, AliasTarget target) {
    if (!object.empty() && !field.empty()) {
        if (target.field_path.empty()) target.field_path.push_back(field);
        bind(object + "." + field, std::move(target));
    }
}

void AliasGraph::bindFieldPath(const std::string& object, const std::vector<std::string>& fields,
                               AliasTarget target) {
    if (object.empty() || fields.empty()) return;
    if (target.field_path.empty()) target.field_path = fields;
    bind(joinAliasPath(object, fields, {}), std::move(target));
}

void AliasGraph::bindArrayElement(const std::string& object, int index, AliasTarget target) {
    if (object.empty() || index < 0) return;
    if (target.index_path.empty()) target.index_path.push_back(index);
    bind(joinAliasPath(object, {}, {index}), std::move(target));
}

std::optional<AliasTarget> AliasGraph::resolve(const std::string& alias) const {
    auto it = aliases_.find(alias);
    if (it == aliases_.end()) return std::nullopt;
    return it->second;
}

std::optional<AliasTarget> AliasGraph::resolveField(const std::string& object, const std::string& field) const {
    return resolve(object + "." + field);
}

std::optional<AliasTarget> AliasGraph::resolvePath(const std::string& object,
                                                   const std::vector<std::string>& fields,
                                                   const std::vector<int>& indices) const {
    return resolve(joinAliasPath(object, fields, indices));
}

bool AliasGraph::has(const std::string& alias) const {
    return aliases_.find(alias) != aliases_.end();
}

void AliasGraph::recordUnsupported(const std::string& message) {
    diagnostics_.push_back(message);
}

} // namespace pred
