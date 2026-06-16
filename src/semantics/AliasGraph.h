#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pred {

enum class AliasKind {
    Value,
    ConstRef,
    MutableRef,
    Pointer,
    FieldRef,
    RegProxyPort,
    ReqHelperPort
};

enum class AliasMutability {
    Unknown,
    Immutable,
    Mutable,
};

struct AliasSourceLoc {
    std::string file;
    int line = 0;
    int column = 0;
};

struct AliasTarget {
    AliasTarget() = default;
    AliasTarget(std::string target_name, AliasKind target_kind, bool target_writable)
        : name(std::move(target_name)),
          canonical_name(name),
          base_object(name),
          kind(target_kind),
          mutability(target_writable ? AliasMutability::Mutable : AliasMutability::Immutable),
          writable(target_writable) {}

    std::string name;
    std::string canonical_name;
    std::string base_object;
    std::vector<std::string> field_path;
    std::vector<int> index_path;
    AliasKind kind = AliasKind::Value;
    AliasMutability mutability = AliasMutability::Unknown;
    bool writable = false;
    AliasSourceLoc source_loc;
    std::string reason;
};

class AliasGraph {
public:
    void bind(const std::string& alias, AliasTarget target);
    void bindReference(const std::string& alias, const std::string& target, bool is_const,
                       std::string reason = {});
    void bindPointer(const std::string& alias, const std::string& target,
                     std::string reason = {});
    void bindField(const std::string& object, const std::string& field, AliasTarget target);
    void bindFieldPath(const std::string& object, const std::vector<std::string>& fields,
                       AliasTarget target);
    void bindArrayElement(const std::string& object, int index, AliasTarget target);

    std::optional<AliasTarget> resolve(const std::string& alias) const;
    std::optional<AliasTarget> resolveField(const std::string& object, const std::string& field) const;
    std::optional<AliasTarget> resolvePath(const std::string& object,
                                           const std::vector<std::string>& fields,
                                           const std::vector<int>& indices = {}) const;
    bool has(const std::string& alias) const;
    void recordUnsupported(const std::string& message);
    const std::vector<std::string>& diagnostics() const { return diagnostics_; }

private:
    std::unordered_map<std::string, AliasTarget> aliases_;
    std::vector<std::string> diagnostics_;
};

} // namespace pred
