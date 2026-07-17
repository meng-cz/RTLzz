#pragma once

#include "debug/DebugLoc.h"

#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pred::v2 {

struct TypeInfo {
    std::string name;
    int width = 0;
    // Source signedness hint for C++ builtin integers or signed views. The
    // RTL storage type Int<N> itself is unsigned; operation operands carry
    // signed interpretation explicitly via signed_view in later stages.
    bool is_signed = false;
    bool is_hw_int = false;
    std::string hw_kind;
    bool is_pointer = false;
    bool is_reference = false;
    bool is_const = false;
    bool is_mutable = true;
    bool is_array = false;
    int array_size = 0;
    std::vector<int> array_dims;
    std::string struct_name;
    bool is_static = false;
    std::vector<std::string> init_values;
};

inline bool is_bool_type_info(const TypeInfo& type) {
    return (type.name == "bool" || type.hw_kind == "bool") &&
           !type.is_pointer && !type.is_reference && !type.is_array &&
           type.struct_name.empty();
}

inline TypeInfo canonical_bool_type() {
    TypeInfo t;
    t.name = "bool";
    t.width = 1;
    t.is_signed = false;
    t.is_hw_int = true;
    t.hw_kind = "bool";
    return t;
}

inline TypeInfo canonicalize_bool_type(TypeInfo type) {
    if (!is_bool_type_info(type)) return type;
    TypeInfo out = canonical_bool_type();
    out.is_const = type.is_const;
    out.is_mutable = type.is_mutable;
    out.is_static = type.is_static;
    out.init_values = std::move(type.init_values);
    return out;
}

enum class ParamPassingKind {
    Value,
    ConstRef,
    MutableRef,
    RValueRef,
    Pointer,
};

enum class ParamDirection {
    Input,
    Output,
};

inline std::string paramDirectionName(ParamDirection direction) {
    switch (direction) {
    case ParamDirection::Input: return "Input";
    case ParamDirection::Output: return "Output";
    }
    return "Input";
}

struct ParamDecl {
    TypeInfo type;
    std::string name;
    DebugLoc debug_loc;
    bool is_output = false;
    ParamDirection direction = ParamDirection::Input;
    ParamPassingKind passing = ParamPassingKind::Value;
    bool is_const = false;
    bool is_pointer = false;
    bool is_reference = false;
};

struct StructFieldInfo {
    std::string name;
    TypeInfo type;
};

struct StructConstructorInfo {
    std::vector<std::string> param_names;
    std::unordered_map<std::string, std::string> field_to_param;
};

inline TypeInfo make_hw_type(const std::string& kind, int width, bool is_signed = false) {
    (void)is_signed;
    TypeInfo t;
    const bool is_bool = kind == "bool";
    const std::string storage_kind = is_bool ? "bool" : "Int";
    t.name = is_bool ? "bool" : storage_kind + "<" + std::to_string(width) + ">";
    t.width = width;
    t.is_hw_int = true;
    t.is_signed = false;
    t.hw_kind = storage_kind;
    return t;
}

inline TypeInfo make_bool_type() {
    return canonical_bool_type();
}

inline TypeInfo make_bits_type(int width, bool is_signed = false) {
    TypeInfo t;
    t.name = "Int<" + std::to_string(width) + ">";
    t.width = width;
    t.is_signed = is_signed;
    t.is_hw_int = true;
    t.hw_kind = "Int";
    return t;
}

inline TypeInfo make_unknown_type(const std::string& name = "") {
    TypeInfo t;
    t.name = name;
    return t;
}

} // namespace pred::v2
