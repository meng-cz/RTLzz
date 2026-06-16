#include "ast/VulTypeRecognizer.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace pred {
namespace {

std::string cxToStdString(CXString s) {
    const char* c = clang_getCString(s);
    std::string out = c ? c : "";
    clang_disposeString(s);
    return out;
}

std::string typeSpelling(CXType type) {
    return cxToStdString(clang_getTypeSpelling(type));
}

std::string canonicalTypeSpelling(CXType type) {
    return cxToStdString(clang_getTypeSpelling(clang_getCanonicalType(type)));
}

std::string stripSpaces(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
        return std::isspace(c);
    }), s.end());
    return s;
}

std::optional<int> parseTemplateWidth(const std::string& text, const std::string& token) {
    std::string compact = stripSpaces(text);
    auto pos = compact.find(token + "<");
    if (pos == std::string::npos) return std::nullopt;
    auto begin = pos + token.size() + 1;
    auto end = compact.find('>', begin);
    if (end == std::string::npos || end <= begin) return std::nullopt;
    std::string value = compact.substr(begin, end - begin);
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c);
        })) {
        return std::nullopt;
    }
    return std::stoi(value);
}

std::optional<int> parseArrayDimAfterComma(const std::string& text) {
    int depth = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '<') ++depth;
        else if (c == '>') --depth;
        else if (c == ',' && depth == 1) {
            size_t j = i + 1;
            while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
            size_t k = j;
            while (k < text.size() && std::isdigit(static_cast<unsigned char>(text[k]))) ++k;
            if (k > j) return std::stoi(text.substr(j, k - j));
        }
    }
    return std::nullopt;
}

std::string parseStdArrayElementText(const std::string& text) {
    auto pos = text.find("array<");
    if (pos == std::string::npos) pos = text.find("array <");
    if (pos == std::string::npos) return "";
    auto lt = text.find('<', pos);
    if (lt == std::string::npos) return "";
    int depth = 0;
    for (size_t i = lt + 1; i < text.size(); ++i) {
        char c = text[i];
        if (c == '<') ++depth;
        else if (c == '>') {
            if (depth == 0) break;
            --depth;
        } else if (c == ',' && depth == 0) {
            std::string elem = text.substr(lt + 1, i - lt - 1);
            while (!elem.empty() && std::isspace(static_cast<unsigned char>(elem.front()))) elem.erase(elem.begin());
            while (!elem.empty() && std::isspace(static_cast<unsigned char>(elem.back()))) elem.pop_back();
            return elem;
        }
    }
    return "";
}

std::optional<TypeInfo> recognizeBuiltinName(const std::string& spelling) {
    std::string n = stripSpaces(spelling);
    TypeInfo t;
    auto builtin = [&](std::string name, int width, bool sign) {
        t.name = std::move(name);
        t.width = width;
        t.is_signed = sign;
        t.is_hw_int = true;
        t.hw_kind = t.name == "bool" ? "bool" : "builtin";
        return t;
    };

    if (n == "bool" || n == "_Bool") return builtin("bool", 1, false);
    if (n == "uint8_t" || n == "std::uint8_t" || n == "unsignedchar") return builtin("uint8_t", 8, false);
    if (n == "int8_t" || n == "std::int8_t" || n == "signedchar") return builtin("int8_t", 8, true);
    if (n == "uint16_t" || n == "std::uint16_t") return builtin("uint16_t", 16, false);
    if (n == "int16_t" || n == "std::int16_t") return builtin("int16_t", 16, true);
    if (n == "uint32_t" || n == "std::uint32_t") return builtin("uint32_t", 32, false);
    if (n == "int32_t" || n == "std::int32_t") return builtin("int32_t", 32, true);
    if (n == "uint64_t" || n == "std::uint64_t") return builtin("uint64_t", 64, false);
    if (n == "int64_t" || n == "std::int64_t") return builtin("int64_t", 64, true);
    if (n == "unsignedint") return builtin("unsigned int", 32, false);
    if (n == "int") return builtin("int", 32, true);
    if (n == "unsignedlonglong") return builtin("unsigned long long", 64, false);
    if (n == "longlong") return builtin("long long", 64, true);
    return std::nullopt;
}

} // namespace

std::optional<TypeInfo> recognizeHwIntType(CXType type) {
    std::string spelling = typeSpelling(type);
    std::string canonical = canonicalTypeSpelling(type);
    std::string combined = spelling + " " + canonical;

    if (auto width = parseTemplateWidth(combined, "IntSignedView")) {
        TypeInfo out = make_hw_type("Int", *width, true);
        out.name = "IntSignedView<" + std::to_string(*width) + ">";
        out.hw_kind = "signed_view";
        return out;
    }
    if (auto width = parseTemplateWidth(combined, "UInt")) {
        return make_hw_type("UInt", *width, false);
    }
    if (auto width = parseTemplateWidth(combined, "Int")) {
        return make_hw_type("Int", *width, false);
    }
    return std::nullopt;
}

std::optional<TypeInfo> recognizeStdArrayType(
    CXType type,
    const std::function<TypeInfo(CXType)>& convert_elem) {
    std::string spelling = typeSpelling(type);
    std::string canonical = canonicalTypeSpelling(type);
    std::string array_text = spelling.find("array") != std::string::npos ? spelling : canonical;
    if (array_text.find("array<") == std::string::npos &&
        array_text.find("array <") == std::string::npos) {
        return std::nullopt;
    }

    auto dim = parseArrayDimAfterComma(array_text);
    if (!dim || *dim <= 0) return std::nullopt;

    TypeInfo elem;
    CXType elem_type = clang_Type_getTemplateArgumentAsType(type, 0);
    if (elem_type.kind != CXType_Invalid) {
        elem = convert_elem(elem_type);
    } else {
        std::string elem_text = parseStdArrayElementText(array_text);
        if (auto hw = parseTemplateWidth(elem_text, "UInt")) {
            elem = make_hw_type("UInt", *hw, false);
        } else if (auto hw = parseTemplateWidth(elem_text, "Int")) {
            elem = make_hw_type("Int", *hw, false);
        } else if (auto builtin = recognizeBuiltinName(elem_text)) {
            elem = *builtin;
        } else {
            elem.name = elem_text;
        }
    }

    TypeInfo out = elem;
    out.is_array = true;
    out.array_size = *dim;
    out.array_dims = {*dim};
    out.array_dims.insert(out.array_dims.end(), elem.array_dims.begin(), elem.array_dims.end());
    out.is_pointer = false;
    return out;
}

std::optional<TypeInfo> recognizeBuiltinFixedWidth(CXType type) {
    if (type.kind == CXType_Record || type.kind == CXType_Elaborated) {
        return std::nullopt;
    }
    std::string spelling = typeSpelling(type);
    std::string canonical = canonicalTypeSpelling(type);

    if (auto exact = recognizeBuiltinName(spelling)) return exact;
    if (auto exact = recognizeBuiltinName(canonical)) return exact;

    TypeInfo t;
    switch (type.kind) {
    case CXType_Bool:
        return make_hw_type("bool", 1, false);
    case CXType_UChar:
    case CXType_Char_U:
        t = TypeInfo{"unsigned char", 8, false, true, "builtin"};
        return t;
    case CXType_SChar:
    case CXType_Char_S:
        t = TypeInfo{"signed char", 8, true, true, "builtin"};
        return t;
    case CXType_UInt:
        t = TypeInfo{"unsigned int", 32, false, true, "builtin"};
        return t;
    case CXType_Int:
        t = TypeInfo{"int", 32, true, true, "builtin"};
        return t;
    case CXType_ULongLong:
        t = TypeInfo{"unsigned long long", 64, false, true, "builtin"};
        return t;
    case CXType_LongLong:
        t = TypeInfo{"long long", 64, true, true, "builtin"};
        return t;
    default:
        break;
    }
    return std::nullopt;
}

std::optional<TypeInfo> recognizeRecordType(CXType type) {
    CXType canonical = clang_getCanonicalType(type);
    if (type.kind != CXType_Record && canonical.kind != CXType_Record &&
        type.kind != CXType_Elaborated) {
        return std::nullopt;
    }
    TypeInfo out;
    out.name = typeSpelling(type);
    out.struct_name = out.name.empty() ? canonicalTypeSpelling(type) : out.name;
    out.width = 0;
    return out;
}

} // namespace pred
