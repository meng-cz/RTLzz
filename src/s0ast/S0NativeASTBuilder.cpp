#include "s0ast/S0NativeASTBuilder.h"
#include "s0ast/S0VulRecognizers.h"
#include <clang-c/Index.h>
#include <cstring>
#include <functional>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace pred::s0ast {
using pred::v2::TypeInfo;
using pred::v2::Expr;
using pred::v2::ExprPtr;
using pred::v2::ExprKind;
using pred::v2::IntrinsicKind;
using pred::v2::Stmt;
using pred::v2::StmtPtr;
using pred::v2::StmtKind;
using pred::v2::CaseClause;
using pred::v2::FunctionAST;
using pred::v2::ParamDecl;
using pred::v2::ParamDirection;
using pred::v2::ParamPassingKind;
using pred::v2::StructFieldInfo;
using pred::v2::StructConstructorInfo;
using pred::v2::canonicalize_bool_type;
using pred::v2::make_array_access;
using pred::v2::make_binary;
using pred::v2::make_dynamic_write_bit;
using pred::v2::make_dynamic_write_slice;
using pred::v2::make_field_access;
using pred::v2::make_hw_type;
using pred::v2::make_literal;
using pred::v2::make_unary;
using pred::v2::make_var;
using pred::v2::make_bool_type;
using pred::v2::make_bits_type;
using pred::v2::make_unknown_type;

// --- Helpers ---

static std::string cxToStr(CXString s) {
    const char* c = clang_getCString(s);
    std::string result = c ? c : "";
    clang_disposeString(s);
    return result;
}

static bool sourceContainsFixintInclude(const std::string& source_file) {
    std::ifstream in(source_file);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str().find("fixint.hpp") != std::string::npos;
}

static bool sourceTextContainsFixintInclude(const std::string& source_text) {
    return source_text.find("fixint.hpp") != std::string::npos;
}

static std::string escapeForQuotedPath(const std::string& path) {
    std::string escaped;
    escaped.reserve(path.size());
    for (char ch : path) {
        if (ch == '\\' || ch == '"') escaped.push_back('\\');
        escaped.push_back(ch);
    }
    return escaped;
}

static std::string includePathForSource(const std::string& source_file) {
    std::string path = source_file;
    try {
        path = std::filesystem::absolute(source_file).string();
    } catch (const std::exception&) {
    }
    return escapeForQuotedPath(path);
}

static const std::unordered_map<std::string, std::vector<StructFieldInfo>>*
    active_struct_fields = nullptr;
static const std::unordered_map<std::string, std::vector<StructConstructorInfo>>*
    active_struct_constructors = nullptr;

static std::string normalizedPath(const std::string& path) {
    try {
        return std::filesystem::weakly_canonical(std::filesystem::absolute(path)).string();
    } catch (const std::exception&) {
        try {
            return std::filesystem::absolute(path).string();
        } catch (const std::exception&) {
            return path;
        }
    }
}

static bool wildcardMatch(const std::string& pattern, const std::string& text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string::npos;
    std::size_t star_text = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern[p] == text[t]) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_text = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++star_text;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

static bool parseVulWidthName(const std::string& name,
                              const std::string& token,
                              int& width) {
    auto pos = name.find(token);
    if (pos == std::string::npos) return false;
    auto lt = name.find('<', pos);
    auto gt = name.find('>', lt == std::string::npos ? 0 : lt);
    if (lt == std::string::npos || gt == std::string::npos || lt >= gt) return false;
    std::string text = name.substr(lt + 1, gt - lt - 1);
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c);
    }), text.end());
    while (!text.empty() &&
           (text.back() == 'u' || text.back() == 'U' ||
            text.back() == 'l' || text.back() == 'L')) {
        text.pop_back();
    }
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return std::isdigit(c);
        })) {
        return false;
    }
    width = std::stoi(text);
    return true;
}

static void markSignedViewIfCursorType(ExprPtr& expr, CXCursor cursor) {
    if (!expr) return;
    std::string spelling = cxToStr(clang_getTypeSpelling(clang_getCursorType(cursor)));
    if (spelling.find("IntSignedView") == std::string::npos) return;
    int width = expr->type.width;
    int parsed_width = 0;
    if (parseVulWidthName(spelling, "IntSignedView", parsed_width) && parsed_width > 0) {
        width = parsed_width;
    }
    expr->type.is_signed = true;
    expr->type.is_hw_int = true;
    expr->type.hw_kind = "signed_view";
    if (width > 0) {
        expr->type.width = width;
        expr->type.name = "IntSignedView<" + std::to_string(width) + ">";
    }
}

static bool cursorTypeMentions(CXCursor cursor, const std::string& token) {
    std::string spelling = cxToStr(clang_getTypeSpelling(clang_getCursorType(cursor)));
    if (spelling.find(token) != std::string::npos) return true;
    CXCursor referenced = clang_getCursorReferenced(cursor);
    if (!clang_Cursor_isNull(referenced)) {
        spelling = cxToStr(clang_getTypeSpelling(clang_getCursorType(referenced)));
        if (spelling.find(token) != std::string::npos) return true;
    }
    return false;
}

static bool cursorMentionsToken(CXCursor cursor, const std::string& token, int depth = 0) {
    if (depth > 6) return false;
    if (cursorTypeMentions(cursor, token)) return true;
    if (cxToStr(clang_getCursorSpelling(cursor)).find(token) != std::string::npos) return true;
    if (cxToStr(clang_getCursorDisplayName(cursor)).find(token) != std::string::npos) return true;
    bool found = false;
    struct State {
        const std::string* token;
        int depth;
        bool* found;
    } state{&token, depth, &found};
    clang_visitChildren(cursor, [](CXCursor c, CXCursor, CXClientData data) {
        auto* state = static_cast<State*>(data);
        if (cursorMentionsToken(c, *state->token, state->depth + 1)) {
            *state->found = true;
            return CXChildVisit_Break;
        }
        return CXChildVisit_Continue;
    }, &state);
    return found;
}

static void forceSignedView(ExprPtr& expr) {
    if (!expr) return;
    int width = expr->type.width;
    if (width <= 0 && expr->kind == ExprKind::Cast) width = expr->cast_type.width;
    if (width <= 0) return;
    TypeInfo signed_type = expr->type;
    signed_type.width = width;
    signed_type.is_signed = true;
    signed_type.is_hw_int = true;
    signed_type.hw_kind = "signed_view";
    signed_type.name = "IntSignedView<" + std::to_string(width) + ">";
    expr->type = signed_type;
    if (expr->kind == ExprKind::Cast) {
        expr->cast_type = signed_type;
    } else if (expr->kind == ExprKind::Trunc ||
               expr->kind == ExprKind::ZExt ||
               expr->kind == ExprKind::SExt) {
        expr->type = signed_type;
    }
}

static bool isSignedViewMemberName(const std::string& name) {
    return name == "sint" || name == "_sint";
}

static ExprPtr unwrapSignedViewMemberAccess(ExprPtr expr) {
    while (expr && expr->kind == ExprKind::FieldAccess &&
           isSignedViewMemberName(expr->field_name) && expr->struct_base) {
        expr = expr->struct_base;
    }
    return expr;
}

static void markSignedViewIfCursorMentions(ExprPtr& expr, CXCursor cursor) {
    expr = unwrapSignedViewMemberAccess(expr);
    markSignedViewIfCursorType(expr, cursor);
    if (cursorMentionsToken(cursor, "IntSignedView") ||
        cursorMentionsToken(cursor, "sint")) {
        forceSignedView(expr);
    }
}

static TypeInfo convertType(CXType t) {
    if (t.kind == CXType_Pointer) {
        CXType pointee = clang_getPointeeType(t);
        TypeInfo ti = convertType(pointee);
        ti.is_pointer = true;
        ti.is_reference = false;
        ti.is_const = clang_isConstQualifiedType(pointee) != 0;
        ti.is_mutable = !ti.is_const;
        return ti;
    }

    if (t.kind == CXType_LValueReference || t.kind == CXType_RValueReference) {
        CXType pointee = clang_getPointeeType(t);
        TypeInfo ti = convertType(pointee);
        ti.is_pointer = false;
        ti.is_reference = true;
        ti.is_const = clang_isConstQualifiedType(pointee) != 0;
        ti.is_mutable = !ti.is_const;
        return ti;
    }

    if (t.kind == CXType_ConstantArray) {
        CXType elem = clang_getArrayElementType(t);
        TypeInfo elem_info = convertType(elem);
        TypeInfo ti = elem_info;
        ti.is_array = true;
        ti.array_size = static_cast<int>(clang_getArraySize(t));
        ti.array_dims = {ti.array_size};
        ti.array_dims.insert(ti.array_dims.end(), elem_info.array_dims.begin(), elem_info.array_dims.end());
        ti.is_pointer = false;
        return ti;
    }

    CXType canonical = clang_getCanonicalType(t);
    if (t.kind == CXType_Enum || canonical.kind == CXType_Enum) {
        CXCursor enum_decl = clang_getTypeDeclaration(t.kind == CXType_Enum ? t : canonical);
        CXType underlying = clang_getEnumDeclIntegerType(enum_decl);
        TypeInfo ti;
        if (underlying.kind != CXType_Invalid) {
            ti = convertType(underlying);
        } else {
            ti.name = "int";
            ti.width = 32;
            ti.is_signed = true;
            ti.is_hw_int = true;
            ti.hw_kind = "builtin";
        }
        if (ti.width <= 0) {
            ti.width = static_cast<int>(clang_Type_getSizeOf(t)) * 8;
            if (ti.width < 0) ti.width = 32;
        }
        TypeInfo out = make_hw_type("Int", ti.width, false);
        out.is_signed = ti.is_signed;
        return out;
    }

    if (auto array = recognizeStdArrayType(t, convertType)) return *array;
    if (auto hw = recognizeHwIntType(t)) return *hw;
    if (auto record = recognizeRecordType(t)) return *record;
    if (auto builtin = recognizeBuiltinFixedWidth(t)) return *builtin;

    TypeInfo ti;
    ti.name = cxToStr(clang_getTypeSpelling(t));
    ti.width = static_cast<int>(clang_Type_getSizeOf(t)) * 8;
    if (ti.width < 0) ti.width = 0;
    ti.is_signed = (t.kind == CXType_Int || t.kind == CXType_Short ||
                    t.kind == CXType_Long || t.kind == CXType_LongLong ||
                    t.kind == CXType_SChar || t.kind == CXType_Char_S);

    return ti;
}

static bool isBuiltinIntegerType(const TypeInfo& type) {
    if (type.is_array || type.is_pointer || type.is_reference ||
        !type.struct_name.empty()) {
        return false;
    }
    return type.hw_kind == "builtin" ||
           type.hw_kind == "bool" ||
           type.name == "bool";
}

static bool isStandardIntegerToTarget(const TypeInfo& type) {
    if (type.is_array || type.is_pointer || type.is_reference ||
        !type.struct_name.empty() || type.width <= 0) {
        return false;
    }
    if (type.name == "bool" || type.hw_kind == "bool") return false;
    return type.hw_kind == "builtin" ||
           type.name == "char" || type.name == "signed char" ||
           type.name == "unsigned char" || type.name == "short" ||
           type.name == "unsigned short" || type.name == "int" ||
           type.name == "unsigned int" || type.name == "long" ||
           type.name == "unsigned long" || type.name == "long long" ||
           type.name == "unsigned long long" || type.name == "uint8_t" ||
           type.name == "uint16_t" || type.name == "uint32_t" ||
           type.name == "uint64_t" || type.name == "int8_t" ||
           type.name == "int16_t" || type.name == "int32_t" ||
           type.name == "int64_t";
}

static TypeInfo hardwareIntegerTypeForStandardTarget(const TypeInfo& type) {
    TypeInfo out = make_hw_type("Int", type.width, false);
    out.is_signed = type.is_signed;
    return out;
}

static bool sameIntegerType(const TypeInfo& lhs, const TypeInfo& rhs) {
    return lhs.width == rhs.width &&
           lhs.is_signed == rhs.is_signed &&
           lhs.hw_kind == rhs.hw_kind &&
           lhs.name == rhs.name;
}

static ExprPtr makeImplicitBuiltinCast(ExprPtr value, const TypeInfo& target) {
    if (!value || !isBuiltinIntegerType(value->type) ||
        !isBuiltinIntegerType(target) || target.width <= 0 ||
        sameIntegerType(value->type, target)) {
        return value;
    }
    auto cast = std::make_shared<Expr>();
    cast->kind = ExprKind::Cast;
    cast->cast_type = target;
    cast->type = target;
    cast->cast_expr = std::move(value);
    return cast;
}

static TypeInfo builtinIntegerPromotion(const TypeInfo& type) {
    if (!isBuiltinIntegerType(type) || (type.name == "bool" && type.width == 1)) {
        if (type.name == "bool") {
            TypeInfo promoted{"int", 32, true, true, "builtin"};
            return promoted;
        }
        return type;
    }
    if (type.width < 32) {
        // The supported Windows/Linux targets use a 32-bit int, which can
        // represent every value of the fixed-width 8/16-bit builtin types.
        return TypeInfo{"int", 32, true, true, "builtin"};
    }
    return type;
}

static ParamPassingKind classifyParamPassing(CXType type) {
    if (type.kind == CXType_Pointer) return ParamPassingKind::Pointer;
    if (type.kind == CXType_RValueReference) return ParamPassingKind::RValueRef;
    if (type.kind == CXType_LValueReference) {
        CXType pointee = clang_getPointeeType(type);
        return clang_isConstQualifiedType(pointee)
            ? ParamPassingKind::ConstRef
            : ParamPassingKind::MutableRef;
    }
    return ParamPassingKind::Value;
}

static bool isMutableParamPassing(ParamPassingKind passing) {
    return passing == ParamPassingKind::MutableRef || passing == ParamPassingKind::Pointer;
}

static bool sameLiteralExpr(const ExprPtr& lhs, const ExprPtr& rhs) {
    return lhs && rhs &&
           lhs->kind == ExprKind::Literal &&
           rhs->kind == ExprKind::Literal &&
           lhs->literal_value == rhs->literal_value;
}

// Forward declarations
static ExprPtr convertExpr(CXCursor cursor);
static ExprPtr convertInitArgExpr(CXCursor cursor, const TypeInfo& fallback_type = {});
static bool isDesignatedInitFieldCursor(CXCursor cursor, int depth = 0);
static ExprPtr convertExprImpl(CXCursor cursor);
static StmtPtr convertStmt(CXCursor cursor);
static StmtPtr convertStmtImpl(CXCursor cursor);
static std::vector<StmtPtr> convertChildren(CXCursor cursor);
static std::string cursorLocation(CXCursor cursor);
static void collectLocalLambdas(CXCursor cursor, FunctionAST& func);
static void collectScopedConstInts(CXCursor root, const std::string& source_file);
static CXCursor referencedOperatorMethodInCursor(CXCursor cursor);
static std::vector<std::string> constIntExprTokens(const std::string& expr);

[[noreturn]] static void failUnsupported(CXCursor cursor, const std::string& message) {
    throw std::runtime_error(message + " at " + cursorLocation(cursor));
}

// Visitor that collects child cursors
struct ChildCollector {
    std::vector<CXCursor> children;
};

static CXChildVisitResult collectChildVisitor(CXCursor cursor, CXCursor, CXClientData data) {
    auto* collector = static_cast<ChildCollector*>(data);
    collector->children.push_back(cursor);
    return CXChildVisit_Continue;
}

static std::vector<CXCursor> getChildren(CXCursor cursor) {
    ChildCollector collector;
    clang_visitChildren(cursor, collectChildVisitor, &collector);
    return collector.children;
}

static std::optional<TypeInfo> operatorReceiverType(CXCursor cursor,
                                                    const std::string& operator_name,
                                                    int depth = 0) {
    if (depth > 8) return std::nullopt;
    CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_DeclRefExpr || kind == CXCursor_MemberRefExpr ||
        kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod) {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (!clang_equalCursors(referenced, clang_getNullCursor())) {
            std::string ref_name = cxToStr(clang_getCursorSpelling(referenced));
            if (ref_name == operator_name) {
                CXCursor parent = clang_getCursorSemanticParent(referenced);
                if (!clang_equalCursors(parent, clang_getNullCursor())) {
                    TypeInfo parent_type = convertType(clang_getCursorType(parent));
                    if (parent_type.is_hw_int && parent_type.width > 0) return parent_type;
                }
            }
        }
    }
    for (auto& child : getChildren(cursor)) {
        if (auto found = operatorReceiverType(child, operator_name, depth + 1)) {
            return found;
        }
    }
    return std::nullopt;
}

static std::string firstSpellingDeep(CXCursor cursor, int depth = 0) {
    std::string spelling = cxToStr(clang_getCursorSpelling(cursor));
    if (!spelling.empty()) return spelling;
    if (depth >= 4) return "";
    auto children = getChildren(cursor);
    if (children.empty()) return "";
    return firstSpellingDeep(children.front(), depth + 1);
}

static unsigned sourceRangeLineSpan(CXSourceRange range) {
    CXFile file;
    unsigned start_line = 0, start_col = 0, start_off = 0;
    unsigned end_line = 0, end_col = 0, end_off = 0;
    clang_getSpellingLocation(clang_getRangeStart(range), &file, &start_line, &start_col, &start_off);
    clang_getSpellingLocation(clang_getRangeEnd(range), &file, &end_line, &end_col, &end_off);
    return end_line >= start_line ? end_line - start_line + 1 : 1;
}

static std::unordered_map<std::string, std::string> lambda_operator_usr_to_name;
static std::unordered_map<std::string, std::string> lambda_operator_location_to_name;
static std::unordered_map<std::string, std::string> lambda_operator_signature_to_name;
static std::unordered_map<std::string, std::string> lambda_var_usr_to_name;
static std::unordered_map<std::string, std::string> lambda_var_location_to_name;
static std::unordered_map<std::string, std::string> lambda_source_name_to_unique_name;
static std::unordered_map<std::string, long long> global_const_int_values;
static std::unordered_map<std::string, long long> current_template_int_values;
static std::unordered_map<std::string, ParamDecl> global_ports_by_usr;
static std::vector<ParamDecl> global_ports_in_source_order;
static std::unordered_map<std::string, std::string> helper_function_usr_to_name;
static std::unordered_map<std::string, std::string> helper_template_call_to_name;
static std::unordered_map<std::string, TypeInfo> function_return_types_by_name;
static std::unordered_map<std::string, TypeInfo> recovered_symbol_types;
static int lambda_unique_counter = 0;

struct FunctionTemplateInfo {
    CXCursor function_decl = clang_getNullCursor();
    std::vector<std::string> template_params;
};

struct PendingFunctionTemplateSpecialization {
    std::string source_name;
    std::string internal_name;
    std::vector<int> args;
};

static std::unordered_map<std::string, FunctionTemplateInfo> source_function_templates_by_name;
static std::unordered_map<std::string, std::string> function_template_specialization_name_by_key;
static std::vector<PendingFunctionTemplateSpecialization>
    pending_function_template_specializations;
static std::unordered_set<std::string> processed_function_template_specializations;

struct LambdaCaptureInfo {
    std::string name;
    TypeInfo type;
    ParamPassingKind passing = ParamPassingKind::Value;
    ParamDirection direction = ParamDirection::Input;
    bool is_reference = false;
    bool is_const = false;
};

static std::unordered_map<std::string, std::vector<LambdaCaptureInfo>> lambda_captures_by_name;
static std::unordered_map<std::string, std::size_t> lambda_param_count_by_name;
static std::unordered_set<std::string> known_lambda_names;
static std::unordered_map<std::string, std::vector<std::vector<int>>>
    lambda_template_specialization_args_by_name;
static std::unordered_map<std::string, std::string> lambda_template_specialization_name_by_key;
static std::unordered_map<std::string, CXCursor> lambda_template_specialization_method_by_key;

static std::string cursorText(CXCursor cursor, bool allow_large = false) {
    CXSourceRange range = clang_getCursorExtent(cursor);
    if (!allow_large && sourceRangeLineSpan(range) > 16) return "";
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, range, &tokens, &numTokens);
    std::string text;
    for (unsigned i = 0; i < numTokens; ++i) {
        if (i > 0) text += " ";
        text += cxToStr(clang_getTokenSpelling(tu, tokens[i]));
    }
    clang_disposeTokens(tu, tokens, numTokens);
    return text;
}

static bool isBinaryOperatorToken(const std::string& token) {
    static const std::unordered_set<std::string> ops = {
        "+", "-", "*", "/", "%", "<<", ">>", "&", "|", "^",
        "&&", "||", "==", "!=", "<", "<=", ">", ">=",
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
    };
    return ops.count(token) != 0;
}

static bool isTemplateArgumentListStart(CXTranslationUnit tu,
                                        CXToken* tokens,
                                        unsigned numTokens,
                                        unsigned index) {
    if (index == 0 || cxToStr(clang_getTokenSpelling(tu, tokens[index])) != "<") {
        return false;
    }
    std::string prev = cxToStr(clang_getTokenSpelling(tu, tokens[index - 1]));
    if (prev != "template" && prev != ">" &&
        (prev.empty() ||
         !(std::isalnum(static_cast<unsigned char>(prev.back())) || prev.back() == '_'))) {
        return false;
    }

    int depth = 0;
    for (unsigned i = index; i < numTokens; ++i) {
        std::string tok = cxToStr(clang_getTokenSpelling(tu, tokens[i]));
        if (tok == "<") {
            ++depth;
        } else if (tok == ">") {
            --depth;
            if (depth == 0) {
                if (i + 1 >= numTokens) return false;
                std::string next = cxToStr(clang_getTokenSpelling(tu, tokens[i + 1]));
                return next == "(" || next == "::" || next == "." || next == "->";
            }
        }
    }
    return false;
}

static std::string fallbackBinaryOperatorFromTokens(CXCursor cursor) {
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, range, &tokens, &numTokens);

    std::string op;
    int paren_depth = 0;
    int bracket_depth = 0;
    int brace_depth = 0;
    for (unsigned i = 0; i < numTokens; ++i) {
        std::string tok = cxToStr(clang_getTokenSpelling(tu, tokens[i]));
        if (tok == "(") {
            ++paren_depth;
        } else if (tok == ")") {
            --paren_depth;
        } else if (tok == "[") {
            ++bracket_depth;
        } else if (tok == "]") {
            --bracket_depth;
        } else if (tok == "{") {
            ++brace_depth;
        } else if (tok == "}") {
            --brace_depth;
        } else if (tok == "<" && isTemplateArgumentListStart(tu, tokens, numTokens, i)) {
            int angle_depth = 1;
            while (++i < numTokens && angle_depth > 0) {
                std::string angle_tok = cxToStr(clang_getTokenSpelling(tu, tokens[i]));
                if (angle_tok == "<") ++angle_depth;
                else if (angle_tok == ">") --angle_depth;
            }
        } else if (paren_depth == 0 && bracket_depth == 0 && brace_depth == 0 &&
                   isBinaryOperatorToken(tok)) {
            op = tok;
            break;
        }
    }

    clang_disposeTokens(tu, tokens, numTokens);
    return op;
}

static std::string binaryOperatorFromCursor(CXCursor cursor,
                                            CXCursor lhs_cursor,
                                            CXCursor rhs_cursor) {
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, range, &tokens, &numTokens);

    CXSourceLocation lhsEnd = clang_getRangeEnd(clang_getCursorExtent(lhs_cursor));
    CXSourceLocation rhsStart = clang_getRangeStart(clang_getCursorExtent(rhs_cursor));
    unsigned lhsEndOffset = 0;
    unsigned rhsStartOffset = 0;
    CXFile f;
    unsigned line = 0;
    unsigned col = 0;
    clang_getSpellingLocation(lhsEnd, &f, &line, &col, &lhsEndOffset);
    clang_getSpellingLocation(rhsStart, &f, &line, &col, &rhsStartOffset);

    std::string op;
    for (unsigned i = 0; i < numTokens; ++i) {
        CXSourceLocation tokLoc = clang_getTokenLocation(tu, tokens[i]);
        unsigned tokOffset = 0;
        clang_getSpellingLocation(tokLoc, &f, &line, &col, &tokOffset);
        if (tokOffset >= lhsEndOffset && tokOffset < rhsStartOffset) {
            std::string candidate = cxToStr(clang_getTokenSpelling(tu, tokens[i]));
            if (isBinaryOperatorToken(candidate)) {
                op = std::move(candidate);
                break;
            }
        }
    }
    clang_disposeTokens(tu, tokens, numTokens);

    if (op.empty()) op = fallbackBinaryOperatorFromTokens(cursor);
    return op.empty() ? "?" : op;
}

static bool containsAny(const std::string& text,
                        const std::vector<std::string>& needles) {
    for (auto& n : needles) {
        if (text.find(n) != std::string::npos) return true;
    }
    return false;
}

static std::string cursorLocation(CXCursor cursor) {
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    CXFile file;
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
    clang_getExpansionLocation(loc, &file, &line, &column, &offset);
    std::string file_name = file ? cxToStr(clang_getFileName(file)) : "<unknown>";
    return file_name + ":" + std::to_string(line) + ":" + std::to_string(column);
}

static DebugLoc debugLocFromCursor(CXCursor cursor) {
    DebugLoc out;
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXFile start_file = nullptr;
    CXFile end_file = nullptr;
    unsigned start_line = 0, start_column = 0, start_offset = 0;
    unsigned end_line = 0, end_column = 0, end_offset = 0;
    clang_getExpansionLocation(clang_getRangeStart(range), &start_file,
                               &start_line, &start_column, &start_offset);
    clang_getExpansionLocation(clang_getRangeEnd(range), &end_file,
                               &end_line, &end_column, &end_offset);
    if (start_file) out.file = cxToStr(clang_getFileName(start_file));
    out.line = static_cast<int>(start_line);
    out.column = static_cast<int>(start_column);
    out.end_line = static_cast<int>(end_line);
    out.end_column = static_cast<int>(end_column);
    return out;
}

static ExprPtr withDebugLoc(ExprPtr expr, CXCursor cursor) {
    if (expr) expr->debug_loc = debugLocFromCursor(cursor);
    return expr;
}

static StmtPtr withDebugLoc(StmtPtr stmt, CXCursor cursor) {
    if (stmt) stmt->debug_loc = debugLocFromCursor(cursor);
    return stmt;
}

static std::string cursorUsr(CXCursor cursor) {
    return cxToStr(clang_getCursorUSR(cursor));
}

static std::string astBaseName(const ExprPtr& e) {
    if (!e) return "";
    if (e->kind == ExprKind::VarRef) return e->var_name;
    if (e->kind == ExprKind::UnaryOp && e->op == "*") return astBaseName(e->operand);
    if (e->kind == ExprKind::Cast) return astBaseName(e->cast_expr);
    if (e->kind == ExprKind::FieldAccess) return astBaseName(e->struct_base);
    if (e->kind == ExprKind::ArrayAccess) return astBaseName(e->array_base);
    return "";
}

static bool isOperatorOnlyExpr(const ExprPtr& e) {
    if (!e) return false;
    if (e->kind == ExprKind::VarRef) return e->var_name.rfind("operator", 0) == 0;
    if (e->kind == ExprKind::FieldAccess) return e->field_name.rfind("operator", 0) == 0 && !e->struct_base;
    if (e->kind == ExprKind::Cast) return isOperatorOnlyExpr(e->cast_expr);
    return false;
}

static ExprPtr makeVulConcatCall(const std::vector<ExprPtr>& args);
static ExprPtr makeVulRepeatCall(int count, ExprPtr value);

static int templateArgIntFromTokens(CXCursor cursor, const std::string& name, int fallback = -1) {
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, range, &tokens, &numTokens);
    int result = fallback;
    for (unsigned i = 0; i + 3 < numTokens; ++i) {
        std::string tok = cxToStr(clang_getTokenSpelling(tu, tokens[i]));
        std::string lt = cxToStr(clang_getTokenSpelling(tu, tokens[i + 1]));
        std::string value = cxToStr(clang_getTokenSpelling(tu, tokens[i + 2]));
        std::string gt = cxToStr(clang_getTokenSpelling(tu, tokens[i + 3]));
        if (tok != name || lt != "<" || gt != ">") continue;
        if (!value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
                return std::isdigit(c);
            })) {
            result = std::stoi(value);
            break;
        }
    }
    clang_disposeTokens(tu, tokens, numTokens);
    return result;
}

static int templateArgInt(CXCursor cursor, int index = 0, int fallback = -1) {
    int count = clang_Cursor_getNumTemplateArguments(cursor);
    if (count <= index) return fallback;
    CXTemplateArgumentKind kind = clang_Cursor_getTemplateArgumentKind(cursor, index);
    if (kind != CXTemplateArgumentKind_Integral) return fallback;
    long long value = clang_Cursor_getTemplateArgumentValue(cursor, index);
    if (value < 0 || value > static_cast<long long>(std::numeric_limits<int>::max())) return fallback;
    return static_cast<int>(value);
}

static std::optional<long long> parseIntegerToken(const std::string& tok) {
    if (tok.empty()) return std::nullopt;
    std::string value = tok;
    while (!value.empty()) {
        char ch = value.back();
        if (ch == 'u' || ch == 'U' || ch == 'l' || ch == 'L') value.pop_back();
        else break;
    }
    if (value.empty()) return std::nullopt;
    try {
        std::size_t pos = 0;
        long long parsed = std::stoll(value, &pos, 0);
        if (pos == value.size()) return parsed;
    } catch (...) {
    }
    return std::nullopt;
}

static std::optional<int> evalTemplateIntTokens(const std::vector<std::string>& tokens) {
    long long result = 0;
    int sign = 1;
    bool saw_value = false;
    bool expect_value = true;
    for (const auto& tok : tokens) {
        if (tok == "+" || tok == "-") {
            if (expect_value && tok == "-") {
                sign *= -1;
            } else {
                sign = tok == "-" ? -1 : 1;
                expect_value = true;
            }
            continue;
        }
        if (tok == "U" || tok == "u" || tok == "L" || tok == "l") continue;
        std::optional<long long> value = parseIntegerToken(tok);
        if (!value) {
            auto template_it = current_template_int_values.find(tok);
            if (template_it != current_template_int_values.end()) value = template_it->second;
        }
        if (!value) {
            auto it = global_const_int_values.find(tok);
            if (it != global_const_int_values.end()) value = it->second;
        }
        if (!value) return std::nullopt;
        result += sign * *value;
        sign = 1;
        saw_value = true;
        expect_value = false;
    }
    if (!saw_value || result < 0 ||
        result > static_cast<long long>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return static_cast<int>(result);
}

static std::string templateIntArgsKey(const std::vector<int>& args) {
    std::string key;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) key += ",";
        key += std::to_string(args[i]);
    }
    return key;
}

static std::string lambdaTemplateSpecializationKey(const std::string& base_name,
                                                   const std::vector<int>& args) {
    return base_name + "<" + templateIntArgsKey(args) + ">";
}

static std::string ensureLambdaTemplateSpecializationName(const std::string& base_name,
                                                          const std::vector<int>& args) {
    std::string key = lambdaTemplateSpecializationKey(base_name, args);
    auto existing = lambda_template_specialization_name_by_key.find(key);
    if (existing != lambda_template_specialization_name_by_key.end()) {
        return existing->second;
    }

    std::string generated = base_name + "__rtlzz_template_" +
                            std::to_string(lambda_template_specialization_name_by_key.size());
    lambda_template_specialization_name_by_key.emplace(key, generated);
    lambda_template_specialization_args_by_name[base_name].push_back(args);
    known_lambda_names.insert(generated);
    return generated;
}

static std::string templateCalleeNameFromText(const std::string& text) {
    std::size_t lt = text.find('<');
    if (lt == std::string::npos) return "";
    std::size_t end = lt;
    while (end > 0 &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    std::size_t begin = end;
    while (begin > 0 &&
           (std::isalnum(static_cast<unsigned char>(text[begin - 1])) ||
            text[begin - 1] == '_' || text[begin - 1] == ':')) {
        --begin;
    }
    std::string name = text.substr(begin, end - begin);
    while (name.rfind("::", 0) == 0) name.erase(0, 2);
    auto scope = name.rfind("::");
    if (scope != std::string::npos) name = name.substr(scope + 2);
    return name;
}

static std::vector<std::string> templateParamNamesFromTemplateText(
    const std::string& text) {
    std::vector<std::string> params;
    std::size_t keyword = text.find("template");
    if (keyword == std::string::npos) return params;
    std::size_t template_begin = text.find('<', keyword);
    if (template_begin == std::string::npos) return params;

    int depth = 0;
    std::size_t template_end = std::string::npos;
    for (std::size_t i = template_begin; i < text.size(); ++i) {
        if (text[i] == '<') ++depth;
        if (text[i] == '>' && --depth == 0) {
            template_end = i;
            break;
        }
    }
    if (template_end == std::string::npos) return params;

    std::string params_text =
        text.substr(template_begin + 1, template_end - template_begin - 1);
    std::size_t begin = 0;
    int nested = 0;
    for (std::size_t i = 0; i <= params_text.size(); ++i) {
        char ch = i < params_text.size() ? params_text[i] : ',';
        if (ch == '<') ++nested;
        if (ch == '>') --nested;
        if (ch != ',' || nested != 0) continue;
        std::string part = params_text.substr(begin, i - begin);
        auto equal = part.find('=');
        if (equal != std::string::npos) part.resize(equal);
        while (!part.empty() &&
               std::isspace(static_cast<unsigned char>(part.back()))) {
            part.pop_back();
        }
        std::size_t name_end = part.size();
        std::size_t name_begin = name_end;
        while (name_begin > 0 &&
               (std::isalnum(static_cast<unsigned char>(part[name_begin - 1])) ||
                part[name_begin - 1] == '_')) {
            --name_begin;
        }
        std::string param_name = part.substr(name_begin, name_end - name_begin);
        if (!param_name.empty()) params.push_back(std::move(param_name));
        begin = i + 1;
    }
    return params;
}

static TypeInfo typeFromSimpleSourceText(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), text.end());
    std::smatch match;
    if (std::regex_search(text, match, std::regex(R"(Int<([0-9]+)>|vulfixint::Int<([0-9]+)>)"))) {
        std::string width_text = match[1].matched ? match[1].str() : match[2].str();
        return make_hw_type("Int", std::stoi(width_text), false);
    }
    if (text == "bool") return TypeInfo{"bool", 1, false};
    if (text == "uint8_t") return TypeInfo{"uint8_t", 8, false, true, "builtin"};
    if (text == "int8_t") return TypeInfo{"int8_t", 8, true, true, "builtin"};
    if (text == "uint16_t") return TypeInfo{"uint16_t", 16, false, true, "builtin"};
    if (text == "int16_t") return TypeInfo{"int16_t", 16, true, true, "builtin"};
    if (text == "uint32_t" || text == "unsignedint") {
        return TypeInfo{"uint32_t", 32, false, true, "builtin"};
    }
    if (text == "int") return TypeInfo{"int", 32, true, true, "builtin"};
    if (text == "uint64_t") return TypeInfo{"uint64_t", 64, false, true, "builtin"};
    if (text == "int64_t") return TypeInfo{"int64_t", 64, true, true, "builtin"};
    return TypeInfo{};
}

static std::vector<ParamDecl> parseFunctionParamsFromTemplateText(
    const std::string& text,
    const std::string& function_name) {
    std::vector<ParamDecl> params;
    std::size_t name_pos = text.find(function_name);
    if (name_pos == std::string::npos) return params;
    std::size_t lparen = text.find('(', name_pos + function_name.size());
    if (lparen == std::string::npos) return params;
    int depth = 0;
    std::size_t rparen = std::string::npos;
    for (std::size_t i = lparen; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        if (text[i] == ')' && --depth == 0) {
            rparen = i;
            break;
        }
    }
    if (rparen == std::string::npos) return params;

    std::string params_text = text.substr(lparen + 1, rparen - lparen - 1);
    std::size_t begin = 0;
    int nested_angle = 0;
    for (std::size_t i = 0; i <= params_text.size(); ++i) {
        char ch = i < params_text.size() ? params_text[i] : ',';
        if (ch == '<') ++nested_angle;
        if (ch == '>') --nested_angle;
        if (ch != ',' || nested_angle != 0) continue;
        std::string part = params_text.substr(begin, i - begin);
        begin = i + 1;
        while (!part.empty() &&
               std::isspace(static_cast<unsigned char>(part.back()))) {
            part.pop_back();
        }
        std::size_t name_end = part.size();
        std::size_t name_begin = name_end;
        while (name_begin > 0 &&
               (std::isalnum(static_cast<unsigned char>(part[name_begin - 1])) ||
                part[name_begin - 1] == '_')) {
            --name_begin;
        }
        if (name_begin == name_end) continue;
        std::string param_name = part.substr(name_begin, name_end - name_begin);
        std::string type_text = part.substr(0, name_begin);
        TypeInfo type = typeFromSimpleSourceText(type_text);
        if (param_name.empty() || type.name.empty()) continue;
        ParamDecl param;
        param.name = std::move(param_name);
        param.type = std::move(type);
        param.passing = ParamPassingKind::Value;
        param.direction = ParamDirection::Input;
        params.push_back(std::move(param));
    }
    return params;
}

static std::string ensureFunctionTemplateSpecializationName(
    const std::string& source_name,
    const std::vector<int>& args) {
    if (source_name.empty() || args.empty()) return "";
    std::string key = lambdaTemplateSpecializationKey(source_name, args);
    auto existing = function_template_specialization_name_by_key.find(key);
    if (existing != function_template_specialization_name_by_key.end()) {
        return existing->second;
    }
    auto template_it = source_function_templates_by_name.find(source_name);
    if (template_it == source_function_templates_by_name.end()) return "";
    if (args.size() != template_it->second.template_params.size()) {
        return "";
    }

    std::string internal_name = source_name + "__rtlzz_template_late_" +
                                std::to_string(function_template_specialization_name_by_key.size());
    function_template_specialization_name_by_key.emplace(key, internal_name);
    helper_template_call_to_name[key] = internal_name;
    function_return_types_by_name[internal_name] =
        convertType(clang_getCursorResultType(template_it->second.function_decl));
    pending_function_template_specializations.push_back(
        PendingFunctionTemplateSpecialization{source_name, internal_name, args});
    return internal_name;
}

static std::string operatorSignature(CXCursor method) {
    CXType result_type = clang_getCursorResultType(method);
    std::string signature = cxToStr(clang_getTypeSpelling(result_type));
    signature += "(";
    int arg_count = clang_Cursor_getNumArguments(method);
    for (int i = 0; i < arg_count; ++i) {
        if (i > 0) signature += ",";
        signature += cxToStr(clang_getTypeSpelling(clang_getArgType(clang_getCursorType(method), i)));
    }
    signature += ")";
    return signature;
}

static std::string normalizeOperatorFunctionType(std::string type) {
    type.erase(std::remove(type.begin(), type.end(), '\r'), type.end());
    type.erase(std::remove(type.begin(), type.end(), '\n'), type.end());
    auto arrow = type.find("->");
    if (arrow != std::string::npos) {
        std::string ret = type.substr(arrow + 2);
        auto comment = ret.find("//");
        if (comment != std::string::npos) ret = ret.substr(0, comment);
        ret.erase(0, ret.find_first_not_of(" \t"));
        auto end = ret.find_last_not_of(" \t");
        if (end == std::string::npos) return "";
        ret.erase(end + 1);
        return ret + "()";
    }
    return "";
}

static std::string sanitizeLambdaNamePart(std::string text) {
    for (char& ch : text) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') ch = '_';
    }
    if (text.empty()) text = "lambda";
    return text;
}

static std::string lambdaVarKey(CXCursor var_cursor) {
    std::string usr = cursorUsr(var_cursor);
    if (!usr.empty()) return "usr:" + usr;
    return "loc:" + cursorLocation(var_cursor);
}

static std::string lambdaNameForVarCursor(CXCursor var_cursor) {
    std::string usr = cursorUsr(var_cursor);
    if (!usr.empty()) {
        auto it = lambda_var_usr_to_name.find(usr);
        if (it != lambda_var_usr_to_name.end()) return it->second;
    }
    std::string loc = cursorLocation(var_cursor);
    auto loc_it = lambda_var_location_to_name.find(loc);
    if (loc_it != lambda_var_location_to_name.end()) return loc_it->second;
    return "";
}

static std::string ensureLambdaNameForVar(CXCursor var_cursor,
                                          const std::string& source_name) {
    std::string existing = lambdaNameForVarCursor(var_cursor);
    if (!existing.empty()) return existing;

    std::string generated = "__s0_lambda_" + sanitizeLambdaNamePart(source_name) +
                            "_" + std::to_string(lambda_unique_counter++);
    std::string usr = cursorUsr(var_cursor);
    if (!usr.empty()) lambda_var_usr_to_name[usr] = generated;
    std::string loc = cursorLocation(var_cursor);
    if (!loc.empty()) lambda_var_location_to_name[loc] = generated;
    auto [source_it, inserted] =
        lambda_source_name_to_unique_name.emplace(source_name, generated);
    if (!inserted && source_it->second != generated) source_it->second.clear();
    known_lambda_names.insert(generated);
    return generated;
}

static std::string lambdaNameForReferencedVar(CXCursor cursor) {
    CXCursor referenced = clang_getCursorReferenced(cursor);
    if (clang_Cursor_isNull(referenced)) return "";
    if (clang_getCursorKind(referenced) != CXCursor_VarDecl) return "";
    return lambdaNameForVarCursor(referenced);
}

static std::string lambdaNameForReceiverCursor(CXCursor cursor) {
    if (auto name = lambdaNameForReferencedVar(cursor); !name.empty()) return name;
    for (auto& child : getChildren(cursor)) {
        if (auto name = lambdaNameForReceiverCursor(child); !name.empty()) return name;
    }
    return "";
}

static std::string lambdaNameFromCallSourceText(CXCursor cursor) {
    std::string text = cursorText(cursor);
    for (std::size_t pos = 0; pos < text.size(); ++pos) {
        if (!(std::isalpha(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) {
            continue;
        }
        std::size_t begin = pos++;
        while (pos < text.size() &&
               (std::isalnum(static_cast<unsigned char>(text[pos])) || text[pos] == '_')) {
            ++pos;
        }
        std::string source_name = text.substr(begin, pos - begin);
        std::size_t after = pos;
        while (after < text.size() &&
               std::isspace(static_cast<unsigned char>(text[after]))) {
            ++after;
        }
        if (after >= text.size() || text[after] != '(') continue;
        auto it = lambda_source_name_to_unique_name.find(source_name);
        if (it != lambda_source_name_to_unique_name.end()) return it->second;
    }
    return "";
}

static void registerLambdaOperatorName(CXCursor lambda_cursor, const std::string& name) {
    if (name.empty()) return;
    known_lambda_names.insert(name);
    clang_visitChildren(lambda_cursor, [](CXCursor c, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto* name = static_cast<const std::string*>(data);
        if (clang_getCursorKind(c) == CXCursor_CXXMethod &&
            cxToStr(clang_getCursorSpelling(c)) == "operator()") {
            std::string usr = cursorUsr(c);
            if (!usr.empty()) lambda_operator_usr_to_name[usr] = *name;
            std::string location = cursorLocation(c);
            if (!location.empty()) lambda_operator_location_to_name[location] = *name;
            std::string sig = operatorSignature(c);
            if (!sig.empty()) {
                auto it = lambda_operator_signature_to_name.find(sig);
                if (it == lambda_operator_signature_to_name.end()) {
                    lambda_operator_signature_to_name.emplace(sig, *name);
                } else if (it->second != *name) {
                    it->second.clear();
                }
            }
            std::string function_sig = normalizeOperatorFunctionType(
                cxToStr(clang_getTypeSpelling(clang_getCursorType(c))));
            if (!function_sig.empty()) {
                auto it = lambda_operator_signature_to_name.find(function_sig);
                if (it == lambda_operator_signature_to_name.end()) {
                    lambda_operator_signature_to_name.emplace(function_sig, *name);
                } else if (it->second != *name) {
                    it->second.clear();
                }
            }
            return CXChildVisit_Break;
        }
        return CXChildVisit_Recurse;
    }, const_cast<std::string*>(&name));
}

static CXCursor findLambdaExpr(CXCursor cursor) {
    if (clang_getCursorKind(cursor) == CXCursor_LambdaExpr) return cursor;
    for (auto& child : getChildren(cursor)) {
        CXCursor found = findLambdaExpr(child);
        if (!clang_equalCursors(found, clang_getNullCursor())) return found;
    }
    return clang_getNullCursor();
}

static void collectLambdaOperatorNames(CXCursor cursor) {
    if (clang_getCursorKind(cursor) == CXCursor_VarDecl) {
        std::string source_name = cxToStr(clang_getCursorSpelling(cursor));
        CXCursor lambda = findLambdaExpr(cursor);
        if (!clang_equalCursors(lambda, clang_getNullCursor())) {
            std::string name = ensureLambdaNameForVar(cursor, source_name);
            registerLambdaOperatorName(lambda, name);
        }
    }
    for (auto& child : getChildren(cursor)) {
        collectLambdaOperatorNames(child);
    }
}

static std::string lambdaNameForOperatorCursor(CXCursor cursor) {
    CXCursor referenced = clang_getCursorReferenced(cursor);
    if (!clang_equalCursors(referenced, clang_getNullCursor()) &&
        cxToStr(clang_getCursorSpelling(referenced)) == "operator()") {
        std::string usr = cursorUsr(referenced);
        auto it = lambda_operator_usr_to_name.find(usr);
        if (it != lambda_operator_usr_to_name.end() && !it->second.empty()) return it->second;
        std::string location = cursorLocation(referenced);
        auto loc_it = lambda_operator_location_to_name.find(location);
        if (loc_it != lambda_operator_location_to_name.end() && !loc_it->second.empty()) return loc_it->second;
    }
    for (auto& child : getChildren(cursor)) {
        std::string name = lambdaNameForOperatorCursor(child);
        if (!name.empty()) return name;
    }
    return "";
}

static std::string lambdaNameForOperatorCallType(CXCursor cursor) {
    std::string type = cxToStr(clang_getTypeSpelling(clang_getCursorType(cursor)));
    std::string sig = normalizeOperatorFunctionType(type);
    if (sig.empty()) return "";
    auto it = lambda_operator_signature_to_name.find(sig);
    if (it != lambda_operator_signature_to_name.end() && !it->second.empty()) return it->second;
    return "";
}

static std::vector<int> staticRangeArgsFromType(CXType type) {
    std::string spelling = cxToStr(clang_getTypeSpelling(type));
    if (spelling.find("StaticRangeProxy") == std::string::npos &&
        spelling.find("VULStaticSliceRef") == std::string::npos &&
        spelling.find("IntStaticRangeProxy") == std::string::npos) {
        return {};
    }
    auto lt = spelling.find('<');
    auto gt = spelling.rfind('>');
    if (lt == std::string::npos || gt == std::string::npos || lt >= gt) return {};
    std::vector<int> values;
    std::stringstream ss(spelling.substr(lt + 1, gt - lt - 1));
    std::string part;
    while (std::getline(ss, part, ',')) {
        part.erase(std::remove_if(part.begin(), part.end(), [](unsigned char c) {
            return std::isspace(c);
        }), part.end());
        try {
            size_t used = 0;
            int value = std::stoi(part, &used, 0);
            if (used == 0) return {};
            values.push_back(value);
        } catch (...) {
            return {};
        }
    }
    if (values.size() != 3) return {};
    return {values[1], values[2]};
}

static std::vector<int> templateIntArgsFromTokens(CXCursor cursor, const std::string& name) {
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, clang_getCursorExtent(cursor), &tokens, &numTokens);
    std::vector<int> values;
    std::vector<std::string> current_arg;
    bool in_template = false;
    bool saw_at = false;
    int nested_angle = 0;
    auto flush_arg = [&]() -> bool {
        if (current_arg.empty()) return true;
        auto value = evalTemplateIntTokens(current_arg);
        current_arg.clear();
        if (!value) return false;
        values.push_back(*value);
        return true;
    };
    for (unsigned i = 0; i < numTokens; ++i) {
        std::string tok = cxToStr(clang_getTokenSpelling(tu, tokens[i]));
        if (!saw_at) {
            saw_at = tok == name;
            continue;
        }
        if (!in_template) {
            if (tok == "<") {
                in_template = true;
            } else if (tok == "(") {
                break;
            }
            continue;
        }
        if (tok == "<") {
            ++nested_angle;
            current_arg.push_back(tok);
            continue;
        }
        if (tok == ">" && nested_angle > 0) {
            --nested_angle;
            current_arg.push_back(tok);
            continue;
        }
        if (tok == ">" && nested_angle == 0) {
            if (!flush_arg()) values.clear();
            break;
        }
        if (tok == "," && nested_angle == 0) {
            if (!flush_arg()) {
                values.clear();
                break;
            }
            continue;
        }
        current_arg.push_back(tok);
    }
    clang_disposeTokens(tu, tokens, numTokens);
    if (!values.empty()) return values;
    return {};
}

static std::vector<int> templateIntArgsFromText(const std::string& text,
                                                const std::string& callee) {
    std::size_t name_pos = callee.empty() ? std::string::npos : text.find(callee);
    std::size_t lt = name_pos == std::string::npos
        ? text.find('<')
        : text.find('<', name_pos + callee.size());
    if (lt == std::string::npos) return {};

    int depth = 0;
    std::size_t gt = std::string::npos;
    for (std::size_t i = lt; i < text.size(); ++i) {
        if (text[i] == '<') ++depth;
        if (text[i] == '>' && --depth == 0) {
            gt = i;
            break;
        }
    }
    if (gt == std::string::npos || gt <= lt + 1) return {};

    std::vector<int> values;
    std::string args_text = text.substr(lt + 1, gt - lt - 1);
    std::size_t begin = 0;
    int nested = 0;
    for (std::size_t i = 0; i <= args_text.size(); ++i) {
        char ch = i < args_text.size() ? args_text[i] : ',';
        if (ch == '<') ++nested;
        if (ch == '>') --nested;
        if (ch != ',' || nested != 0) continue;
        auto value = evalTemplateIntTokens(
            constIntExprTokens(args_text.substr(begin, i - begin)));
        if (!value) return {};
        values.push_back(*value);
        begin = i + 1;
    }
    return values;
}

static TypeInfo typeForCallResult(CXType cursor_type, const std::string& callee) {
    TypeInfo result = convertType(cursor_type);
    if ((result.name.empty() || result.width <= 0) && !callee.empty()) {
        auto it = function_return_types_by_name.find(callee);
        if (it != function_return_types_by_name.end()) {
            result = it->second;
        }
    }
    return result;
}

static ExprPtr makeSurfaceCall(std::string callee,
                               TypeInfo type,
                               std::vector<ExprPtr> args = {},
                               DebugLoc loc = {}) {
    auto result = std::make_shared<Expr>();
    result->kind = ExprKind::Call;
    result->callee = std::move(callee);
    result->type = std::move(type);
    result->args = std::move(args);
    result->debug_loc = std::move(loc);
    return result;
}

static ExprPtr makeSurfaceCallFromCursors(std::string callee,
                                          TypeInfo type,
                                          const std::vector<CXCursor>& cursors,
                                          DebugLoc loc = {}) {
    std::vector<ExprPtr> args;
    args.reserve(cursors.size());
    for (auto cursor : cursors) args.push_back(convertExpr(cursor));
    return makeSurfaceCall(std::move(callee), std::move(type), std::move(args), std::move(loc));
}

static bool hasParamNamed(const FunctionAST& fn, const std::string& name) {
    return std::any_of(fn.params.begin(), fn.params.end(), [&](const ParamDecl& param) {
        return param.name == name;
    });
}

static bool isLambdaClosureCapture(CXCursor capture_ref) {
    return !lambdaNameForReferencedVar(capture_ref).empty();
}

static ParamDirection directionForCapturePassing(ParamPassingKind passing) {
    return passing == ParamPassingKind::MutableRef || passing == ParamPassingKind::Pointer
        ? ParamDirection::Output
        : ParamDirection::Input;
}

static std::optional<LambdaCaptureInfo> captureInfoFromRefAndField(CXCursor ref,
                                                                   CXCursor field) {
    if (isLambdaClosureCapture(ref)) return std::nullopt;
    CXCursor referenced = clang_getCursorReferenced(ref);
    if (clang_Cursor_isNull(referenced)) return std::nullopt;
    CXCursorKind referenced_kind = clang_getCursorKind(referenced);
    if (referenced_kind != CXCursor_VarDecl && referenced_kind != CXCursor_ParmDecl) {
        return std::nullopt;
    }
    std::string name = cxToStr(clang_getCursorSpelling(ref));
    if (name.empty()) return std::nullopt;

    CXType field_type = clang_Cursor_isNull(field)
        ? clang_getCursorType(ref)
        : clang_getCursorType(field);
    LambdaCaptureInfo capture;
    capture.name = name;
    capture.type = convertType(field_type);
    capture.passing = classifyParamPassing(field_type);
    capture.direction = directionForCapturePassing(capture.passing);
    capture.is_reference = capture.passing == ParamPassingKind::ConstRef ||
                           capture.passing == ParamPassingKind::MutableRef;
    capture.is_const = capture.type.is_const;
    return capture;
}

static std::vector<LambdaCaptureInfo> collectLambdaCaptures(CXCursor lambda_cursor) {
    std::vector<CXCursor> fields;
    std::vector<CXCursor> refs;
    for (auto& child : getChildren(lambda_cursor)) {
        CXCursorKind kind = clang_getCursorKind(child);
        if (kind == CXCursor_FieldDecl) {
            fields.push_back(child);
        } else if (kind == CXCursor_DeclRefExpr) {
            refs.push_back(child);
        }
    }

    std::vector<LambdaCaptureInfo> captures;
    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        CXCursor field = i < fields.size() ? fields[i] : clang_getNullCursor();
        auto capture = captureInfoFromRefAndField(refs[i], field);
        if (!capture || seen.count(capture->name)) continue;
        seen.insert(capture->name);
        captures.push_back(std::move(*capture));
    }
    return captures;
}

static ParamDecl paramFromCapture(const LambdaCaptureInfo& capture) {
    ParamDecl param;
    param.name = capture.name;
    param.type = capture.type;
    param.passing = capture.passing;
    param.direction = capture.direction;
    param.is_output = param.direction != ParamDirection::Input;
    param.is_reference = capture.is_reference;
    param.is_const = capture.is_const;
    param.is_pointer = capture.passing == ParamPassingKind::Pointer;
    return param;
}

static bool appendCaptureParam(FunctionAST& fn, const LambdaCaptureInfo& capture) {
    if (hasParamNamed(fn, capture.name)) return false;
    fn.params.push_back(paramFromCapture(capture));
    return true;
}

static bool addTransitiveCaptureParam(FunctionAST& fn,
                                      const std::string& lambda_name,
                                      const LambdaCaptureInfo& capture) {
    if (hasParamNamed(fn, capture.name)) return false;
    auto& captures = lambda_captures_by_name[lambda_name];
    std::size_t insert_pos = captures.size();
    captures.push_back(capture);
    auto param = paramFromCapture(capture);
    if (insert_pos > fn.params.size()) insert_pos = fn.params.size();
    fn.params.insert(fn.params.begin() + static_cast<std::ptrdiff_t>(insert_pos),
                     std::move(param));
    lambda_param_count_by_name[lambda_name] = fn.params.size();
    return true;
}

static ExprPtr makeCaptureArg(const LambdaCaptureInfo& capture, DebugLoc loc = {}) {
    auto arg = make_var(capture.name, capture.type);
    arg->debug_loc = std::move(loc);
    return arg;
}

static std::vector<ExprPtr> lambdaCaptureArgs(const std::string& lambda_name,
                                              DebugLoc loc = {}) {
    std::vector<ExprPtr> args;
    auto it = lambda_captures_by_name.find(lambda_name);
    if (it == lambda_captures_by_name.end()) return args;
    args.reserve(it->second.size());
    for (const auto& capture : it->second) {
        args.push_back(makeCaptureArg(capture, loc));
    }
    return args;
}

static void prependLambdaCaptureArgs(const std::string& lambda_name,
                                     std::vector<ExprPtr>& args,
                                     DebugLoc loc = {}) {
    auto captures = lambdaCaptureArgs(lambda_name, std::move(loc));
    if (captures.empty()) return;
    captures.insert(captures.end(), args.begin(), args.end());
    args = std::move(captures);
}

static StmtPtr makeAssignStmtAst(ExprPtr target, ExprPtr value) {
    auto s = std::make_shared<Stmt>();
    s->kind = StmtKind::Assign;
    s->assign_target = std::move(target);
    s->assign_value = std::move(value);
    return s;
}

static StmtPtr makeBlockStmtAst(std::vector<StmtPtr> stmts) {
    auto s = std::make_shared<Stmt>();
    s->kind = StmtKind::Block;
    s->block_stmts = std::move(stmts);
    return s;
}

static std::string compactText(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char c) {
        return std::isspace(c);
    }), text.end());
    return text;
}

static bool isOperatorSpelling(const std::string& spelling, const std::string& op) {
    return compactText(spelling) == "operator" + op;
}

static bool isSimpleIdentifierText(const std::string& s) {
    if (s.empty()) return false;
    auto first = static_cast<unsigned char>(s.front());
    if (!(std::isalpha(first) || s.front() == '_')) return false;
    for (char ch : s) {
        auto c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || ch == '_')) return false;
    }
    return true;
}

static std::optional<std::string> simpleParenthesizedIdentifier(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), text.end());
    if (text.size() < 3 || text.front() != '(' || text.back() != ')') {
        return std::nullopt;
    }
    std::string inner = text.substr(1, text.size() - 2);
    if (!isSimpleIdentifierText(inner)) return std::nullopt;
    return inner;
}

static bool isEmptyParenText(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), text.end());
    return text == "()";
}

static bool isParenIdentifierListText(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), text.end());
    if (text.size() < 3 || text.front() != '(' || text.back() != ')') return false;
    std::string inner = text.substr(1, text.size() - 2);
    if (inner.empty()) return true;
    std::size_t begin = 0;
    bool saw = false;
    for (std::size_t i = 0; i <= inner.size(); ++i) {
        if (i < inner.size() && inner[i] != ',') continue;
        std::string part = inner.substr(begin, i - begin);
        if (!isSimpleIdentifierText(part)) return false;
        saw = true;
        begin = i + 1;
    }
    return saw;
}

static ExprPtr parseSimpleIntConstructText(std::string text) {
    text.erase(std::remove_if(text.begin(), text.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), text.end());
    while (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
        text = text.substr(1, text.size() - 2);
    }
    if (text.rfind("Int<", 0) != 0) return nullptr;
    auto gt = text.find('>');
    if (gt == std::string::npos) return nullptr;
    std::string width_text = text.substr(4, gt - 4);
    if (width_text.empty() ||
        !std::all_of(width_text.begin(), width_text.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        })) {
        return nullptr;
    }
    if (gt + 1 >= text.size() || text[gt + 1] != '(' || text.back() != ')') return nullptr;
    int width = std::stoi(width_text);
    std::string inner = text.substr(gt + 2, text.size() - gt - 3);
    TypeInfo target = make_hw_type("Int", width, false);
    if (isSimpleIdentifierText(inner)) {
        auto cast = std::make_shared<Expr>();
        cast->kind = ExprKind::Cast;
        cast->cast_type = target;
        cast->type = target;
        cast->cast_expr = make_var(inner, make_unknown_type());
        return cast;
    }
    bool numeric = !inner.empty() &&
                   std::all_of(inner.begin(), inner.end(), [](unsigned char ch) {
                       return std::isdigit(ch);
                   });
    if (numeric) return make_literal(inner, target);
    return nullptr;
}

static ExprPtr parseSimpleRecoveredArgText(std::string text) {
    text.erase(0, text.find_first_not_of(" \t\r\n"));
    auto end = text.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) text.erase(end + 1);
    while (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
        int depth = 0;
        bool wraps_all = true;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '(') ++depth;
            if (text[i] == ')') --depth;
            if (depth == 0 && i + 1 < text.size()) {
                wraps_all = false;
                break;
            }
        }
        if (!wraps_all) break;
        text = text.substr(1, text.size() - 2);
    }
    if (isSimpleIdentifierText(text)) {
        auto known = recovered_symbol_types.find(text);
        return make_var(text, known == recovered_symbol_types.end()
                                  ? make_unknown_type()
                                  : known->second);
    }
    if (!text.empty() &&
        std::all_of(text.begin(), text.end(), [](unsigned char ch) {
            return std::isdigit(ch);
        })) {
        return make_literal(text, TypeInfo{"uint32_t", 32, false, true, "builtin"});
    }
    if (auto int_construct = parseSimpleIntConstructText(text)) return int_construct;
    for (const std::string& op : {"^", "+", "-"}) {
        int paren_depth = 0;
        int bracket_depth = 0;
        int angle_depth = 0;
        std::size_t pos = std::string::npos;
        for (std::size_t i = 0; i < text.size(); ++i) {
            char ch = text[i];
            if (ch == '(') ++paren_depth;
            else if (ch == ')') --paren_depth;
            else if (ch == '[') ++bracket_depth;
            else if (ch == ']') --bracket_depth;
            else if (ch == '<') ++angle_depth;
            else if (ch == '>') --angle_depth;
            else if (paren_depth == 0 && bracket_depth == 0 &&
                     angle_depth == 0 && text.compare(i, op.size(), op) == 0) {
                pos = i;
                break;
            }
        }
        if (pos == std::string::npos) continue;
        auto lhs = parseSimpleRecoveredArgText(text.substr(0, pos));
        auto rhs = parseSimpleRecoveredArgText(text.substr(pos + op.size()));
        if (lhs && rhs) {
            auto known_type = [](const TypeInfo& type) {
                return type.width > 0 || type.is_array ||
                       !type.struct_name.empty() ||
                       (!type.name.empty() && type.name != "unknown");
            };
            TypeInfo type = known_type(lhs->type)
                ? lhs->type
                : (known_type(rhs->type) ? rhs->type : make_unknown_type());
            return make_binary(op, lhs, rhs, type);
        }
    }
    if (!text.empty() && text.back() == ']') {
        int depth = 0;
        std::size_t lbracket = std::string::npos;
        for (std::size_t i = text.size(); i-- > 0;) {
            if (text[i] == ']') ++depth;
            if (text[i] == '[') {
                --depth;
                if (depth == 0) {
                    lbracket = i;
                    break;
                }
            }
        }
        if (lbracket != std::string::npos) {
            auto base = parseSimpleRecoveredArgText(text.substr(0, lbracket));
            auto index = parseSimpleRecoveredArgText(
                text.substr(lbracket + 1, text.size() - lbracket - 2));
            if (base && index) {
                TypeInfo element_type = base->type;
                if (element_type.is_array && !element_type.array_dims.empty()) {
                    element_type.array_dims.erase(element_type.array_dims.begin());
                    element_type.is_array = !element_type.array_dims.empty();
                    element_type.array_size = element_type.is_array
                        ? element_type.array_dims.front()
                        : 0;
                }
                return make_array_access(base, index, element_type);
            }
        }
    }
    return nullptr;
}

static ExprPtr wrapRecoveredCallWithTrailingBinary(ExprPtr call, const std::string& text) {
    if (!call) return call;
    std::size_t lparen = std::string::npos;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '(') continue;
        std::size_t before = i;
        while (before > 0 && std::isspace(static_cast<unsigned char>(text[before - 1]))) --before;
        if (before > 0) {
            char prev = text[before - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_') {
                lparen = i;
                break;
            }
        }
    }
    if (lparen == std::string::npos) return call;
    int depth = 0;
    std::size_t rparen = std::string::npos;
    for (std::size_t i = lparen; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        if (text[i] == ')') {
            --depth;
            if (depth == 0) {
                rparen = i;
                break;
            }
        }
    }
    if (rparen == std::string::npos) return call;
    std::size_t op_pos = rparen + 1;
    while (op_pos < text.size() && std::isspace(static_cast<unsigned char>(text[op_pos]))) ++op_pos;
    if (op_pos >= text.size()) return call;
    std::string op(1, text[op_pos]);
    if (op != "+" && op != "-" && op != "^") return call;
    auto rhs = parseSimpleRecoveredArgText(text.substr(op_pos + 1));
    if (!rhs) return call;
    TypeInfo type = (call->type.name.empty() || call->type.name == "unknown")
        ? rhs->type
        : call->type;
    return make_binary(op, std::move(call), std::move(rhs), type);
}

static void appendSimpleParenArgFromCursor(std::vector<ExprPtr>& args, CXCursor cursor) {
    if (auto identifier = simpleParenthesizedIdentifier(cursorText(cursor))) {
        args.push_back(make_var(*identifier, make_unknown_type()));
    }
}

static std::optional<std::string> singleIdentifierCallArgFromText(const std::string& text) {
    std::size_t lparen = std::string::npos;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '(') continue;
        std::size_t before = i;
        while (before > 0 && std::isspace(static_cast<unsigned char>(text[before - 1]))) {
            --before;
        }
        if (before > 0) {
            char prev = text[before - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_') {
                lparen = i;
                break;
            }
        }
    }
    if (lparen == std::string::npos) {
        return std::nullopt;
    }
    std::size_t rparen = std::string::npos;
    int depth = 0;
    for (std::size_t i = lparen; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        if (text[i] == ')') {
            --depth;
            if (depth == 0) {
                rparen = i;
                break;
            }
        }
    }
    if (rparen == std::string::npos || lparen >= rparen) return std::nullopt;
    std::string inner = text.substr(lparen + 1, rparen - lparen - 1);
    inner.erase(std::remove_if(inner.begin(), inner.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), inner.end());
    if (!isSimpleIdentifierText(inner)) return std::nullopt;
    return inner;
}

static void appendSingleIdentifierCallArgFromText(std::vector<ExprPtr>& args,
                                                  const std::string& text) {
    if (auto name = singleIdentifierCallArgFromText(text)) {
        args.push_back(make_var(*name, make_unknown_type()));
    }
}

static void appendSingleIdentifierCallArgFromCursor(std::vector<ExprPtr>& args, CXCursor cursor) {
    appendSingleIdentifierCallArgFromText(args, cursorText(cursor));
}

static void appendIdentifierCallArgsFromText(std::vector<ExprPtr>& args,
                                             const std::string& text) {
    std::size_t lparen = std::string::npos;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '(') continue;
        std::size_t before = i;
        while (before > 0 && std::isspace(static_cast<unsigned char>(text[before - 1]))) --before;
        if (before > 0) {
            char prev = text[before - 1];
            if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_') {
                lparen = i;
                break;
            }
        }
    }
    if (lparen == std::string::npos) return;
    int depth = 0;
    std::size_t rparen = std::string::npos;
    for (std::size_t i = lparen; i < text.size(); ++i) {
        if (text[i] == '(') ++depth;
        if (text[i] == ')') {
            --depth;
            if (depth == 0) {
                rparen = i;
                break;
            }
        }
    }
    if (rparen == std::string::npos || rparen <= lparen + 1) return;
    std::string inner = text.substr(lparen + 1, rparen - lparen - 1);
    std::size_t begin = 0;
    depth = 0;
    for (std::size_t i = 0; i <= inner.size(); ++i) {
        char ch = i < inner.size() ? inner[i] : ',';
        if (ch == '(' || ch == '[' || ch == '<') ++depth;
        if (ch == ')' || ch == ']' || ch == '>') --depth;
        if (ch != ',' || depth != 0) continue;
        std::string part = inner.substr(begin, i - begin);
        part.erase(std::remove_if(part.begin(), part.end(), [](unsigned char c) {
            return std::isspace(c);
        }), part.end());
        if (!part.empty()) {
            if (auto arg = parseSimpleRecoveredArgText(part)) {
                args.push_back(std::move(arg));
            }
        }
        begin = i + 1;
    }
}

static void appendIdentifierCallArgsFromCursor(std::vector<ExprPtr>& args, CXCursor cursor) {
    appendIdentifierCallArgsFromText(args, cursorText(cursor));
}

static void collectWrittenBases(const std::vector<StmtPtr>& stmts,
                                std::unordered_set<std::string>& out) {
    for (auto& s : stmts) {
        if (!s) continue;
        if (s->kind == StmtKind::Assign) {
            std::string name = astBaseName(s->assign_target);
            if (!name.empty()) out.insert(name);
        } else if (s->kind == StmtKind::If) {
            collectWrittenBases(s->if_then, out);
            collectWrittenBases(s->if_else, out);
        } else if (s->kind == StmtKind::For) {
            collectWrittenBases(s->for_body, out);
        } else if (s->kind == StmtKind::Switch) {
            for (auto& c : s->switch_cases) collectWrittenBases(c.body, out);
        } else if (s->kind == StmtKind::Block) {
            collectWrittenBases(s->block_stmts, out);
        }
    }
}

static void collectReadBasesExpr(const ExprPtr& e, std::unordered_set<std::string>& out) {
    if (!e) return;
    switch (e->kind) {
    case ExprKind::VarRef:
        if (!e->var_name.empty()) out.insert(e->var_name);
        return;
    case ExprKind::BinaryOp:
        collectReadBasesExpr(e->left, out);
        collectReadBasesExpr(e->right, out);
        return;
    case ExprKind::UnaryOp:
        collectReadBasesExpr(e->operand, out);
        return;
    case ExprKind::ArrayAccess:
        collectReadBasesExpr(e->array_base, out);
        collectReadBasesExpr(e->index, out);
        return;
    case ExprKind::FieldAccess:
        collectReadBasesExpr(e->struct_base, out);
        return;
    case ExprKind::Call:
        for (auto& arg : e->args) collectReadBasesExpr(arg, out);
        return;
    case ExprKind::Cast:
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc:
        collectReadBasesExpr(e->cast_expr, out);
        return;
    case ExprKind::Ternary:
        collectReadBasesExpr(e->cond, out);
        collectReadBasesExpr(e->then_expr, out);
        collectReadBasesExpr(e->else_expr, out);
        return;
    case ExprKind::Slice:
    case ExprKind::BitSelect:
        collectReadBasesExpr(e->base, out);
        return;
    case ExprKind::WriteSlice:
    case ExprKind::WriteBit:
        collectReadBasesExpr(e->base, out);
        collectReadBasesExpr(e->value, out);
        return;
    case ExprKind::DynamicWriteSlice:
    case ExprKind::DynamicWriteBit:
        collectReadBasesExpr(e->base, out);
        collectReadBasesExpr(e->index, out);
        collectReadBasesExpr(e->value, out);
        return;
    case ExprKind::Concat:
        for (auto& part : e->parts) collectReadBasesExpr(part, out);
        return;
    case ExprKind::Repeat:
    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor:
        collectReadBasesExpr(e->operand, out);
        return;
    case ExprKind::Literal:
        return;
    }
}

static void collectReadBasesFromAssignmentTarget(const ExprPtr& e,
                                                 std::unordered_set<std::string>& out) {
    if (!e) return;
    switch (e->kind) {
    case ExprKind::ArrayAccess:
        collectReadBasesFromAssignmentTarget(e->array_base, out);
        collectReadBasesExpr(e->index, out);
        return;
    case ExprKind::FieldAccess:
        collectReadBasesFromAssignmentTarget(e->struct_base, out);
        return;
    case ExprKind::Slice:
    case ExprKind::BitSelect:
        collectReadBasesFromAssignmentTarget(e->base, out);
        return;
    case ExprKind::WriteSlice:
    case ExprKind::WriteBit:
        collectReadBasesFromAssignmentTarget(e->base, out);
        collectReadBasesExpr(e->value, out);
        return;
    case ExprKind::DynamicWriteSlice:
    case ExprKind::DynamicWriteBit:
        collectReadBasesFromAssignmentTarget(e->base, out);
        collectReadBasesExpr(e->index, out);
        collectReadBasesExpr(e->value, out);
        return;
    default:
        return;
    }
}

static void collectReadBases(const std::vector<StmtPtr>& stmts,
                             std::unordered_set<std::string>& out) {
    for (auto& s : stmts) {
        if (!s) continue;
        switch (s->kind) {
        case StmtKind::Assign:
            collectReadBasesFromAssignmentTarget(s->assign_target, out);
            collectReadBasesExpr(s->assign_value, out);
            break;
        case StmtKind::Decl:
            if (s->decl_init.has_value()) collectReadBasesExpr(s->decl_init.value(), out);
            for (auto& arg : s->decl_init_args) collectReadBasesExpr(arg, out);
            break;
        case StmtKind::If:
            collectReadBasesExpr(s->if_cond, out);
            collectReadBases(s->if_then, out);
            collectReadBases(s->if_else, out);
            break;
        case StmtKind::For:
            if (s->for_init) collectReadBases({s->for_init}, out);
            collectReadBasesExpr(s->for_cond, out);
            collectReadBasesExpr(s->for_step, out);
            collectReadBases(s->for_body, out);
            break;
        case StmtKind::While:
        case StmtKind::DoWhile:
            collectReadBasesExpr(s->while_cond, out);
            collectReadBases(s->while_body, out);
            break;
        case StmtKind::Switch:
            collectReadBasesExpr(s->switch_expr, out);
            for (auto& c : s->switch_cases) {
                if (c.value.has_value()) collectReadBasesExpr(c.value.value(), out);
                collectReadBases(c.body, out);
            }
            break;
        case StmtKind::Block:
            collectReadBases(s->block_stmts, out);
            break;
        case StmtKind::Return:
            if (s->return_value.has_value()) collectReadBasesExpr(s->return_value.value(), out);
            break;
        case StmtKind::ExprStmt:
            collectReadBasesExpr(s->expr_stmt, out);
            break;
        case StmtKind::Break:
        case StmtKind::Continue:
            break;
        }
    }
}

static ParamDirection inferParamDirection(const ParamDecl& p) {
    if (p.passing == ParamPassingKind::Value || p.passing == ParamPassingKind::ConstRef) {
        return ParamDirection::Input;
    }
    if (p.passing == ParamPassingKind::MutableRef) {
        return ParamDirection::Output;
    }
    return ParamDirection::Input;
}

static ParamDecl makeParamDeclFromCursor(CXCursor cursor) {
    ParamDecl p;
    p.name = cxToStr(clang_getCursorSpelling(cursor));
    p.debug_loc = debugLocFromCursor(cursor);
    CXType type = clang_getCursorType(cursor);
    p.type = convertType(type);
    p.passing = classifyParamPassing(type);
    p.is_const = p.type.is_const;
    p.is_pointer = p.passing == ParamPassingKind::Pointer;
    p.is_reference = p.passing == ParamPassingKind::ConstRef ||
                     p.passing == ParamPassingKind::MutableRef;
    p.direction = inferParamDirection(p);
    p.is_output = p.direction != ParamDirection::Input;
    return p;
}

static std::string invalidTopParamReason(const ParamDecl& p) {
    if (p.passing == ParamPassingKind::Pointer) {
        return "unsupported pointer parameter '" + p.name +
               "': top-level ports must be value/const-reference inputs or non-const-reference outputs";
    }
    if (p.passing == ParamPassingKind::RValueRef) {
        return "unsupported rvalue-reference parameter '" + p.name +
               "': top-level ports must be value/const-reference inputs or non-const-reference outputs";
    }
    return "";
}

static bool astBodyEndsWithSwitchTerminator(const std::vector<StmtPtr>& body) {
    if (body.empty()) return false;
    auto last = body.back();
    if (!last) return false;
    if (last->kind == StmtKind::Break || last->kind == StmtKind::Return) return true;
    if (last->kind == StmtKind::Block) return astBodyEndsWithSwitchTerminator(last->block_stmts);
    return false;
}

static bool switchHasFallthrough(CXCursor switch_cursor) {
    CXSourceRange range = clang_getCursorExtent(switch_cursor);
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(switch_cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, range, &tokens, &numTokens);

    struct TokInfo {
        std::string text;
        int brace_depth = 0;
    };
    std::vector<TokInfo> toks;
    toks.reserve(numTokens);
    int depth = 0;
    for (unsigned i = 0; i < numTokens; ++i) {
        std::string t = cxToStr(clang_getTokenSpelling(tu, tokens[i]));
        if (t == "}") --depth;
        toks.push_back(TokInfo{t, depth});
        if (t == "{") ++depth;
    }
    clang_disposeTokens(tu, tokens, numTokens);

    std::vector<size_t> labels;
    for (size_t i = 0; i < toks.size(); ++i) {
        if ((toks[i].text == "case" || toks[i].text == "default") && toks[i].brace_depth == 1) {
            labels.push_back(i);
        }
    }
    if (labels.empty()) return false;

    for (size_t li = 0; li < labels.size(); ++li) {
        size_t start = labels[li];
        size_t end = li + 1 < labels.size() ? labels[li + 1] : toks.size();
        while (end > start && (toks[end - 1].text == "}" || toks[end - 1].text == ";")) {
            if (toks[end - 1].text == "}" && toks[end - 1].brace_depth == 0) break;
            if (toks[end - 1].text == ";") break;
            --end;
        }

        bool terminated = false;
        for (size_t i = end; i-- > start;) {
            if (toks[i].brace_depth != 1) continue;
            if (toks[i].text == ";" && i > start) continue;
            terminated = toks[i].text == "break" || toks[i].text == "return";
            break;
        }
        if (!terminated) return true;
    }
    return false;
}

static std::string cursorFileName(CXCursor cursor) {
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    CXFile file;
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
    clang_getExpansionLocation(loc, &file, &line, &column, &offset);
    return file ? cxToStr(clang_getFileName(file)) : "";
}

static void collectSourceFunctionTemplateDefinitions(CXCursor root,
                                                     const std::string& source_file) {
    struct Ctx {
        std::string source_file;
    } ctx{normalizedPath(source_file)};

    clang_visitChildren(root, [](CXCursor c, CXCursor,
                                 CXClientData data) -> CXChildVisitResult {
        auto* ctx = static_cast<Ctx*>(data);
        if (clang_getCursorKind(c) != CXCursor_FunctionTemplate) {
            return CXChildVisit_Recurse;
        }
        if (normalizedPath(cursorFileName(c)) == ctx->source_file) {
            std::string source_name = cxToStr(clang_getCursorSpelling(c));
            auto template_params = templateParamNamesFromTemplateText(cursorText(c, true));
            if (!source_name.empty()) {
                source_function_templates_by_name[source_name] =
                    FunctionTemplateInfo{c, template_params};
            }
        }

        struct PrimaryCtx {
            std::string source_file;
            std::vector<std::string> template_params;
        } primary_ctx{ctx->source_file,
                      templateParamNamesFromTemplateText(cursorText(c, true))};

        clang_visitChildren(c, [](CXCursor child, CXCursor,
                                  CXClientData child_data) -> CXChildVisitResult {
            auto* primary_ctx = static_cast<PrimaryCtx*>(child_data);
            if (clang_getCursorKind(child) != CXCursor_FunctionDecl ||
                !clang_isCursorDefinition(child) ||
                normalizedPath(cursorFileName(child)) != primary_ctx->source_file ||
                !clang_Cursor_isNull(clang_getSpecializedCursorTemplate(child))) {
                return CXChildVisit_Continue;
            }
            std::string source_name = cxToStr(clang_getCursorSpelling(child));
            if (!source_name.empty()) {
                source_function_templates_by_name[source_name] =
                    FunctionTemplateInfo{child, primary_ctx->template_params};
            }
            return CXChildVisit_Break;
        }, &primary_ctx);
        return CXChildVisit_Recurse;
    }, &ctx);
}

struct FunctionCursorInfo {
    std::string name;
    CXCursor cursor;
};

static std::vector<FunctionCursorInfo> collectSourceFunctionDefinitions(CXCursor root,
                                                                        const std::string& source_file) {
    struct Ctx {
        std::string source_file;
        std::vector<FunctionCursorInfo> functions;
    } ctx{source_file, {}};

    clang_visitChildren(root, [](CXCursor c, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto* ctx = static_cast<Ctx*>(data);
        if (clang_getCursorKind(c) == CXCursor_FunctionDecl && clang_isCursorDefinition(c)) {
            if (cursorFileName(c) == ctx->source_file) {
                std::string name = cxToStr(clang_getCursorSpelling(c));
                function_return_types_by_name[name] =
                    convertType(clang_getCursorResultType(c));
                ctx->functions.push_back({name, c});
            }
        } else if (clang_getCursorKind(c) == CXCursor_FunctionTemplate) {
            struct PrimaryTemplateCtx {
                std::vector<std::string> template_params;
                std::string source_file;
            } primary_ctx{templateParamNamesFromTemplateText(cursorText(c, true)),
                          ctx->source_file};
            clang_visitChildren(c, [](CXCursor child, CXCursor,
                                      CXClientData data) -> CXChildVisitResult {
                auto* primary_ctx = static_cast<PrimaryTemplateCtx*>(data);
                if (clang_getCursorKind(child) != CXCursor_FunctionDecl ||
                    !clang_isCursorDefinition(child) ||
                    cursorFileName(child) != primary_ctx->source_file ||
                    !clang_Cursor_isNull(clang_getSpecializedCursorTemplate(child))) {
                    return CXChildVisit_Continue;
                }
                std::string source_name = cxToStr(clang_getCursorSpelling(child));
                if (!source_name.empty()) {
                    source_function_templates_by_name[source_name] =
                        FunctionTemplateInfo{child, primary_ctx->template_params};
                }
                return CXChildVisit_Break;
            }, &primary_ctx);

            struct TemplateCtx {
                Ctx* outer;
                int specialization_index = 0;
            } template_ctx{ctx, 0};
            clang_visitChildren(c, [](CXCursor child, CXCursor,
                                      CXClientData child_data) -> CXChildVisitResult {
                auto* template_ctx = static_cast<TemplateCtx*>(child_data);
                if (clang_getCursorKind(child) != CXCursor_FunctionDecl ||
                    !clang_isCursorDefinition(child)) {
                    return CXChildVisit_Continue;
                }
                std::string source_name = cxToStr(clang_getCursorSpelling(child));
                CXCursor primary = clang_getSpecializedCursorTemplate(child);
                if (clang_Cursor_isNull(primary)) {
                if (!source_name.empty()) {
                    source_function_templates_by_name[source_name] =
                        FunctionTemplateInfo{
                            child,
                            templateParamNamesFromTemplateText(cursorText(
                                clang_getCursorSemanticParent(child), true))};
                }
                    return CXChildVisit_Continue;
                }
                std::string internal_name = source_name + "__rtlzz_template_" +
                    std::to_string(template_ctx->specialization_index++);
                helper_function_usr_to_name[cursorUsr(child)] = internal_name;
                function_return_types_by_name[internal_name] =
                    convertType(clang_getCursorResultType(child));
                int template_arg_count = clang_Cursor_getNumTemplateArguments(child);
                if (template_arg_count > 0) {
                    std::string call_key = source_name + "<";
                    for (int i = 0; i < template_arg_count; ++i) {
                        if (i) call_key += ",";
                        call_key += std::to_string(
                            clang_Cursor_getTemplateArgumentValue(child, i));
                    }
                    call_key += ">";
                    helper_template_call_to_name[call_key] = internal_name;
                }
                template_ctx->outer->functions.push_back({internal_name, child});
                return CXChildVisit_Continue;
            }, &template_ctx);
        }
        return CXChildVisit_Continue;
    }, &ctx);

    // libclang does not consistently enumerate instantiated FunctionDecl
    // cursors as direct children of FunctionTemplate.  Calls do, however,
    // reference the concrete specialization, so collect those definitions
    // from call sites as well.
    struct ReferencedTemplateCtx {
        Ctx* outer;
        std::unordered_set<std::string> seen_usrs;
        int specialization_index = 0;
    } referenced_ctx{&ctx, {}, 0};
    for (const auto& function : ctx.functions) {
        referenced_ctx.seen_usrs.insert(cursorUsr(function.cursor));
    }
    clang_visitChildren(root, [](CXCursor c, CXCursor,
                                 CXClientData data) -> CXChildVisitResult {
        auto* ctx = static_cast<ReferencedTemplateCtx*>(data);
        if (clang_getCursorKind(c) != CXCursor_CallExpr) {
            return CXChildVisit_Recurse;
        }
        CXCursor referenced = clang_getCursorReferenced(c);
        if (clang_Cursor_isNull(referenced) ||
            clang_getCursorKind(referenced) != CXCursor_FunctionDecl ||
            !clang_isCursorDefinition(referenced) ||
            cursorFileName(referenced) != ctx->outer->source_file ||
            clang_Cursor_isNull(clang_getSpecializedCursorTemplate(referenced))) {
            return CXChildVisit_Recurse;
        }
        std::string usr = cursorUsr(referenced);
        if (!ctx->seen_usrs.insert(usr).second) return CXChildVisit_Recurse;
        std::string source_name = cxToStr(clang_getCursorSpelling(referenced));
        std::string internal_name = source_name + "__rtlzz_template_call_" +
            std::to_string(ctx->specialization_index++);
        helper_function_usr_to_name[usr] = internal_name;
        function_return_types_by_name[internal_name] =
            convertType(clang_getCursorResultType(referenced));
        int template_arg_count = clang_Cursor_getNumTemplateArguments(referenced);
        if (template_arg_count > 0) {
            std::string call_key = source_name + "<";
            for (int i = 0; i < template_arg_count; ++i) {
                if (i) call_key += ",";
                call_key += std::to_string(
                    clang_Cursor_getTemplateArgumentValue(referenced, i));
            }
            call_key += ">";
            helper_template_call_to_name[call_key] = internal_name;
        }
        ctx->outer->functions.push_back({internal_name, referenced});
        return CXChildVisit_Recurse;
    }, &referenced_ctx);
    return ctx.functions;
}

static std::string checkSubset(CXCursor function_cursor, const std::string& current_function) {
    struct CheckCtx {
        std::string current_function;
        std::string error;
    } ctx{current_function, ""};

    clang_visitChildren(function_cursor, [](CXCursor c, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto* ctx = static_cast<CheckCtx*>(data);
        if (!ctx->error.empty()) return CXChildVisit_Break;

        CXCursorKind kind = clang_getCursorKind(c);
        std::string spelling = cxToStr(clang_getCursorSpelling(c));
        std::string type = cxToStr(clang_getTypeSpelling(clang_getCursorType(c)));
        std::string text = cursorText(c); // Diagnostics only, not lowering.
        auto unsupported = [&](const std::string& reason) {
            ctx->error = cursorLocation(c) + ": Unsupported C++ subset feature: " + reason;
        };

        if (kind == CXCursor_CXXNewExpr) {
            unsupported("new");
            return CXChildVisit_Break;
        }
        if (kind == CXCursor_SwitchStmt && switchHasFallthrough(c)) {
            unsupported("switch fall-through; every case/default must end with break or return");
            return CXChildVisit_Break;
        }
        if (kind == CXCursor_FloatingLiteral || containsAny(type, {"float", "double", "long double"})) {
            unsupported("floating point");
            return CXChildVisit_Break;
        }
        if (kind == CXCursor_CXXDeleteExpr) {
            unsupported("delete");
            return CXChildVisit_Break;
        }
        if (kind == CXCursor_CXXThrowExpr || kind == CXCursor_CXXTryStmt ||
            kind == CXCursor_CXXCatchStmt) {
            unsupported("exceptions");
            return CXChildVisit_Break;
        }
        if (kind == CXCursor_CXXMethod && clang_CXXMethod_isVirtual(c)) {
            unsupported("virtual function");
            return CXChildVisit_Break;
        }
        if (kind == CXCursor_CallExpr) {
            if (spelling == ctx->current_function) {
                unsupported("recursion");
                return CXChildVisit_Break;
            }
            if (spelling == "malloc" || spelling == "free") {
                unsupported(spelling);
                return CXChildVisit_Break;
            }
            if (containsAny(text, {"std :: cout", "std :: cin", "printf", "scanf", "fprintf", "iostream"})) {
                unsupported("I/O");
                return CXChildVisit_Break;
            }
        }
        if ((kind == CXCursor_BinaryOperator || kind == CXCursor_CompoundAssignOperator) &&
            containsAny(type, {"*", "[]"})) {
            std::string no_space = text;
            no_space.erase(std::remove_if(no_space.begin(), no_space.end(), [](unsigned char ch) {
                return std::isspace(ch);
            }), no_space.end());
            if (containsAny(no_space, {"+", "-"})) {
                unsupported("pointer arithmetic");
                return CXChildVisit_Break;
            }
        }
        if (kind == CXCursor_VarDecl && type.find("&") != std::string::npos &&
            type.find("const") == std::string::npos) {
            unsupported("complex reference alias");
            return CXChildVisit_Break;
        }
        if ((kind == CXCursor_VarDecl || kind == CXCursor_ParmDecl) &&
            containsAny(type, {"(*", "function<"})) {
            unsupported("function pointer");
            return CXChildVisit_Break;
        }
        if (containsAny(type, {"vector", "map", "unordered_map", "deque", "list", "set", "queue"})) {
            unsupported("STL container " + type);
            return CXChildVisit_Break;
        }
        if (containsAny(type, {"volatile", "atomic"})) {
            unsupported("volatile/atomic");
            return CXChildVisit_Break;
        }
        return CXChildVisit_Recurse;
    }, &ctx);

    return ctx.error;
}

struct CallEdge {
    std::string callee;
    CXCursor call_cursor;
};

static std::vector<CallEdge> collectDirectFunctionCalls(CXCursor function_cursor) {
    struct Ctx {
        std::vector<CallEdge> calls;
    } ctx;
    clang_visitChildren(function_cursor, [](CXCursor c, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto* ctx = static_cast<Ctx*>(data);
        if (clang_getCursorKind(c) == CXCursor_CallExpr) {
            CXCursor referenced = clang_getCursorReferenced(c);
            std::string name;
            if (!clang_Cursor_isNull(referenced)) {
                name = cxToStr(clang_getCursorSpelling(referenced));
            }
            if (name.empty()) {
                name = cxToStr(clang_getCursorSpelling(c));
            }
            if (!name.empty()) {
                ctx->calls.push_back({name, c});
            }
        }
        return CXChildVisit_Recurse;
    }, &ctx);
    return ctx.calls;
}

static std::string checkHelperCallGraphRecursion(const std::vector<FunctionCursorInfo>& functions) {
    std::unordered_set<std::string> names;
    for (const auto& fn : functions) {
        if (!fn.name.empty()) names.insert(fn.name);
    }

    std::unordered_map<std::string, std::vector<CallEdge>> graph;
    for (const auto& fn : functions) {
        for (const auto& edge : collectDirectFunctionCalls(fn.cursor)) {
            if (names.count(edge.callee)) {
                graph[fn.name].push_back(edge);
            }
        }
    }

    std::unordered_map<std::string, int> color;
    std::vector<std::string> stack;
    std::function<std::string(const std::string&)> dfs = [&](const std::string& name) -> std::string {
        color[name] = 1;
        stack.push_back(name);
        for (const auto& edge : graph[name]) {
            int next_color = color[edge.callee];
            if (next_color == 1) {
                auto it = std::find(stack.begin(), stack.end(), edge.callee);
                std::ostringstream cycle;
                if (it != stack.end()) {
                    bool first = true;
                    for (; it != stack.end(); ++it) {
                        if (!first) cycle << " -> ";
                        cycle << *it;
                        first = false;
                    }
                    cycle << " -> " << edge.callee;
                } else {
                    cycle << name << " -> " << edge.callee;
                }
                return cursorLocation(edge.call_cursor) +
                       ": Unsupported C++ subset feature: recursive helper call cycle: " +
                       cycle.str();
            }
            if (next_color == 0) {
                auto error = dfs(edge.callee);
                if (!error.empty()) return error;
            }
        }
        stack.pop_back();
        color[name] = 2;
        return "";
    };

    for (const auto& fn : functions) {
        if (color[fn.name] == 0) {
            auto error = dfs(fn.name);
            if (!error.empty()) return error;
        }
    }
    return "";
}

static ExprPtr convertExprImpl(CXCursor cursor) {
    CXCursorKind kind = clang_getCursorKind(cursor);
    CXType type = clang_getCursorType(cursor);
    auto children = getChildren(cursor);

    std::string cursor_type_spelling = cxToStr(clang_getTypeSpelling(type));
    if (cursor_type_spelling.find("IntSignedView") != std::string::npos && !children.empty()) {
        ExprPtr candidate;
        for (auto& child : children) {
            candidate = convertExpr(child);
            candidate = unwrapSignedViewMemberAccess(candidate);
            if (candidate && !isOperatorOnlyExpr(candidate)) break;
        }
        if (candidate) {
            int width = candidate->type.width;
            int parsed_width = 0;
            if (parseVulWidthName(cursor_type_spelling, "IntSignedView", parsed_width) && parsed_width > 0) {
                width = parsed_width;
            }
            if (width > 0) {
                candidate->type.width = width;
                forceSignedView(candidate);
                return candidate;
            }
        }
    }

    switch (kind) {
    case CXCursor_IntegerLiteral: {
        // Get the literal value from tokens
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
        CXToken* tokens = nullptr;
        unsigned numTokens = 0;
        clang_tokenize(tu, range, &tokens, &numTokens);
        std::string val = "0";
        if (numTokens > 0) {
            val = cxToStr(clang_getTokenSpelling(tu, tokens[0]));
        }
        clang_disposeTokens(tu, tokens, numTokens);
        return make_literal(val, convertType(type));
    }

    case CXCursor_FloatingLiteral: {
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
        CXToken* tokens = nullptr;
        unsigned numTokens = 0;
        clang_tokenize(tu, range, &tokens, &numTokens);
        std::string val = "0.0";
        if (numTokens > 0) {
            val = cxToStr(clang_getTokenSpelling(tu, tokens[0]));
        }
        clang_disposeTokens(tu, tokens, numTokens);
        return make_literal(val, convertType(type));
    }

    case CXCursor_CXXBoolLiteralExpr: {
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
        CXToken* tokens = nullptr;
        unsigned numTokens = 0;
        clang_tokenize(tu, range, &tokens, &numTokens);
        std::string val = "false";
        if (numTokens > 0) {
            val = cxToStr(clang_getTokenSpelling(tu, tokens[0]));
        }
        clang_disposeTokens(tu, tokens, numTokens);
        return make_literal(val, TypeInfo{"bool", 1, false});
    }

    case CXCursor_InitListExpr: {
        TypeInfo init_type = convertType(type);
        if (children.empty()) {
            if (!init_type.struct_name.empty() ||
                (!init_type.name.empty() && !isBuiltinIntegerType(init_type))) {
                auto call = std::make_shared<Expr>();
                call->kind = ExprKind::Call;
                call->callee = init_type.struct_name.empty() ? init_type.name : init_type.struct_name;
                call->type = init_type;
                return call;
            }
            if (init_type.name == "bool" || init_type.hw_kind == "bool") {
                return make_literal("false", TypeInfo{"bool", 1, false});
            }
            return make_literal("0", init_type);
        }
        if (children.size() == 1) return convertExpr(children.front());
        auto call = std::make_shared<Expr>();
        call->kind = ExprKind::Call;
        call->callee = init_type.struct_name.empty() ? init_type.name : init_type.struct_name;
        call->type = init_type;
        for (const auto& child : children) {
            if (isDesignatedInitFieldCursor(child)) continue;
            call->args.push_back(convertInitArgExpr(child));
        }
        return call;
    }

    case CXCursor_DeclRefExpr:
    case CXCursor_MemberRefExpr: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        CXCursorKind referenced_kind = clang_Cursor_isNull(referenced)
            ? CXCursor_InvalidFile
            : clang_getCursorKind(referenced);
        if (referenced_kind == CXCursor_EnumConstantDecl) {
            TypeInfo enum_type = convertType(type);
            std::string value;
            if (enum_type.is_signed) {
                value = std::to_string(clang_getEnumConstantDeclValue(referenced));
            } else {
                value = std::to_string(clang_getEnumConstantDeclUnsignedValue(referenced));
            }
            return make_literal(value, enum_type);
        }
        if (kind == CXCursor_MemberRefExpr && !children.empty()) {
            auto base = convertExpr(children[0]);
            std::string field = cxToStr(clang_getCursorSpelling(cursor));
            TypeInfo field_type;
            const bool callable_member =
                referenced_kind == CXCursor_CXXMethod ||
                referenced_kind == CXCursor_FunctionDecl ||
                referenced_kind == CXCursor_FunctionTemplate ||
                referenced_kind == CXCursor_Constructor ||
                referenced_kind == CXCursor_ConversionFunction;
            if (!callable_member &&
                field != "cat" &&
                field != "repeat" && field != "reduce_or" && field != "reduce_and" &&
                field != "reduce_xor" && field != "range_at" && field != "bit_at" &&
                field != "at" && field != "pick" &&
                field != "sint" &&
                field != "operator()" &&
                field.rfind("operator", 0) != 0) {
                field_type = convertType(type);
            }
            return make_field_access(base, field, field_type);
        }
        std::string name = cxToStr(clang_getCursorSpelling(cursor));
        auto template_value = current_template_int_values.find(name);
        if (template_value != current_template_int_values.end()) {
            TypeInfo template_type = convertType(type);
            if (template_type.name.empty() || template_type.width <= 0) {
                template_type = TypeInfo{"uint32_t", 32, false, true, "builtin"};
            }
            return make_literal(std::to_string(template_value->second), template_type);
        }
        if (name.empty()) {
            CXEvalResult evaluated = clang_Cursor_Evaluate(cursor);
            if (evaluated) {
                CXEvalResultKind eval_kind = clang_EvalResult_getKind(evaluated);
                if (eval_kind == CXEval_Int) {
                    TypeInfo evaluated_type = convertType(type);
                    std::string value = evaluated_type.is_signed
                        ? std::to_string(clang_EvalResult_getAsLongLong(evaluated))
                        : std::to_string(clang_EvalResult_getAsUnsigned(evaluated));
                    clang_EvalResult_dispose(evaluated);
                    return make_literal(value, evaluated_type);
                }
                clang_EvalResult_dispose(evaluated);
            }
        }
        auto result = make_var(name, convertType(type));
        if (referenced_kind == CXCursor_VarDecl) {
            auto port = global_ports_by_usr.find(cursorUsr(referenced));
            if (port != global_ports_by_usr.end()) {
                result->global_port_name = port->second.name;
            }
        }
        return result;
    }

    case CXCursor_ArraySubscriptExpr: {
        if (children.size() >= 2) {
            auto base = convertExpr(children[0]);
            auto idx = convertExpr(children[1]);
            TypeInfo elem_type = convertType(type);
            if (base && base->type.is_array && !base->type.array_dims.empty()) {
                elem_type = base->type;
                elem_type.array_dims.erase(elem_type.array_dims.begin());
                elem_type.is_array = !elem_type.array_dims.empty();
                elem_type.array_size = elem_type.is_array ? elem_type.array_dims.front() : 0;
            }
            return make_array_access(base, idx, elem_type);
        }
        break;
    }

    case CXCursor_BinaryOperator:
    case CXCursor_CompoundAssignOperator: {
        if (children.size() >= 2) {
            std::string op = binaryOperatorFromCursor(cursor, children[0], children[1]);
            auto lhs = convertExpr(children[0]);
            auto rhs = convertExpr(children[1]);
            markSignedViewIfCursorMentions(lhs, children[0]);
            markSignedViewIfCursorMentions(rhs, children[1]);
            return make_binary(op, lhs, rhs, convertType(type));
        }
        break;
    }

    case CXCursor_UnaryOperator: {
        if (!children.empty()) {
            CXSourceRange range = clang_getCursorExtent(cursor);
            CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
            CXToken* tokens = nullptr;
            unsigned numTokens = 0;
            clang_tokenize(tu, range, &tokens, &numTokens);
            std::string op = "?";
            if (numTokens > 0) {
                // Prefix: first token is operator; Postfix: last token
                std::string first = cxToStr(clang_getTokenSpelling(tu, tokens[0]));
                if (first == "!" || first == "~" || first == "-" || first == "++" ||
                    first == "--" || first == "*" || first == "&") {
                    op = first;
                } else if (numTokens > 1) {
                    op = cxToStr(clang_getTokenSpelling(tu, tokens[numTokens - 1]));
                }
            }
            clang_disposeTokens(tu, tokens, numTokens);
            auto operand = convertExpr(children[0]);
            return make_unary(op, operand, convertType(type));
        }
        break;
    }

    case CXCursor_ConditionalOperator: {
        if (children.size() >= 3) {
            auto cond = convertExpr(children[0]);
            auto t = convertExpr(children[1]);
            auto f = convertExpr(children[2]);
            return make_ternary(cond, t, f, convertType(type));
        }
        break;
    }

    case CXCursor_CallExpr: {
        const std::string call_text = cursorText(cursor);
        std::string spelling = cxToStr(clang_getCursorSpelling(cursor));
        CXCursor referenced_function = clang_getCursorReferenced(cursor);
        if (!clang_Cursor_isNull(referenced_function)) {
            auto specialized =
                helper_function_usr_to_name.find(cursorUsr(referenced_function));
            if (specialized != helper_function_usr_to_name.end()) {
                spelling = specialized->second;
            }
        }
        if (spelling.empty()) {
            spelling = templateCalleeNameFromText(call_text);
        }
        if (!spelling.empty()) {
            auto template_values = templateIntArgsFromTokens(cursor, ")");
            if (template_values.empty()) {
                template_values = templateIntArgsFromTokens(cursor, spelling);
            }
            if (template_values.empty()) {
                template_values = templateIntArgsFromText(call_text, spelling);
            }
            if (!template_values.empty()) {
                std::string call_key = spelling + "<";
                for (std::size_t i = 0; i < template_values.size(); ++i) {
                    if (i) call_key += ",";
                    call_key += std::to_string(template_values[i]);
                }
                call_key += ">";
                auto specialized = helper_template_call_to_name.find(call_key);
                if (specialized != helper_template_call_to_name.end()) {
                    spelling = specialized->second;
                } else if (auto late =
                               ensureFunctionTemplateSpecializationName(spelling,
                                                                        template_values);
                           !late.empty()) {
                    spelling = late;
                }
            }
        }
        bool spelling_was_empty = spelling.empty();
        if (spelling.empty()) {
            CXCursor referenced = clang_getCursorReferenced(cursor);
            if (!clang_Cursor_isNull(referenced)) {
                spelling = cxToStr(clang_getCursorSpelling(referenced));
            }
        }
        std::string first_child_spelling;
        if (!children.empty()) {
            first_child_spelling = firstSpellingDeep(children[0]);
            if (first_child_spelling.rfind("operator", 0) == 0) {
                if (spelling.empty()) spelling = first_child_spelling;
            }
        }
        if (call_text.find(". template operator ( ) <") != std::string::npos ||
            call_text.find(".template operator()<") != std::string::npos) {
            std::string callee;
            std::size_t receiver_index = children.size();
            for (std::size_t i = 0; i < children.size(); ++i) {
                callee = lambdaNameForReceiverCursor(children[i]);
                if (!callee.empty()) {
                    receiver_index = i;
                    break;
                }
            }
            if (callee.empty()) {
                failUnsupported(cursor,
                                "S0 cannot resolve explicit generic lambda operator() receiver");
            }
            auto template_args = templateIntArgsFromTokens(cursor, ")");
            if (!template_args.empty()) {
                std::string base_callee = callee;
                std::string spec_key =
                    lambdaTemplateSpecializationKey(base_callee, template_args);
                CXCursor referenced = referencedOperatorMethodInCursor(cursor);
                if (!clang_Cursor_isNull(referenced)) {
                    lambda_template_specialization_method_by_key[spec_key] = referenced;
                }
                callee = ensureLambdaTemplateSpecializationName(base_callee, template_args);
            }
            auto result = std::make_shared<Expr>();
            result->kind = ExprKind::Call;
            result->callee = callee;
            result->type = typeForCallResult(type, callee);
            result->literal_value = call_text;
            for (std::size_t i = 0; i < children.size(); ++i) {
                if (i == receiver_index ||
                    !clang_isExpression(clang_getCursorKind(children[i]))) {
                    continue;
                }
                auto arg = convertExpr(children[i]);
                if (arg && !isOperatorOnlyExpr(arg)) {
                    result->args.push_back(std::move(arg));
                }
            }
            if (result->args.empty()) {
                appendIdentifierCallArgsFromText(result->args, call_text);
            }
            prependLambdaCaptureArgs(callee, result->args, debugLocFromCursor(cursor));
            return result;
        }
        VulCallInfo vul_call = recognizeVulCall(cursor, children, spelling, first_child_spelling);
        ExprPtr first_call_child;
        auto get_first_call_child = [&]() -> ExprPtr {
            if (!first_call_child && !children.empty()) first_call_child = convertExpr(children.front());
            return first_call_child;
        };
        if (spelling.rfind("operator", 0) == 0 && spelling != "operator()" &&
            clang_Cursor_getNumArguments(cursor) == 0 &&
            children.size() == 1) {
            auto converted = get_first_call_child();
            TypeInfo target_type = convertType(type);
            if (converted && converted->kind == ExprKind::FieldAccess &&
                converted->field_name == spelling && converted->struct_base) {
                converted = converted->struct_base;
            }
            if (converted && target_type.width > 0 &&
                !target_type.is_array && target_type.struct_name.empty() &&
                !converted->type.is_array &&
                converted->type.struct_name.empty()) {
                if (converted->type.width <= 0) {
                    converted->type = target_type;
                } else if (converted->type.width != target_type.width ||
                           converted->type.is_signed != target_type.is_signed ||
                           converted->type.hw_kind != target_type.hw_kind) {
                    auto cast = std::make_shared<Expr>();
                    cast->kind = ExprKind::Cast;
                    cast->cast_type = target_type;
                    cast->type = target_type;
                    cast->cast_expr = converted;
                    return cast;
                }
            }
            if (converted) return converted;
        }
        if (spelling.rfind("operator", 0) == 0 && spelling != "operator()" &&
            children.size() == 1 && first_child_spelling == spelling) {
            auto converted = get_first_call_child();
            TypeInfo target_type = convertType(type);
            if (converted && converted->kind == ExprKind::FieldAccess &&
                converted->field_name == spelling && converted->struct_base) {
                converted = converted->struct_base;
            }
            if (converted &&
                converted->intrinsic == IntrinsicKind::DynamicRangeAt &&
                converted->type.width <= 0 &&
                target_type.width > 0) {
                converted->type = target_type;
            }
            return converted;
        }
        if (spelling == "operator()" && children.size() == 1 && first_child_spelling == spelling) {
            std::string token_callee;
            int arg_count = clang_Cursor_getNumArguments(cursor);
            int receiver_arg_index = -1;
            for (int i = 0; i < arg_count; ++i) {
                CXCursor arg_cursor = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
                std::string receiver_callee = lambdaNameForReceiverCursor(arg_cursor);
                if (!receiver_callee.empty()) {
                    if (token_callee.empty() || token_callee == "operator()") {
                        token_callee = receiver_callee;
                    }
                    if (receiver_callee == token_callee) {
                        receiver_arg_index = i;
                        break;
                    }
                }
            }
            if (token_callee.empty() || token_callee == "operator()") {
                token_callee = lambdaNameFromCallSourceText(cursor);
            }
            if (!token_callee.empty() && token_callee != "operator()") {
                auto result = std::make_shared<Expr>();
                result->kind = ExprKind::Call;
                result->callee = token_callee;
                result->type = convertType(type);
                result->literal_value = cursorText(cursor);
                for (int i = 0; i < arg_count; ++i) {
                    if (i == receiver_arg_index) continue;
                    CXCursor arg_cursor = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
                    if (isEmptyParenText(cursorText(arg_cursor))) continue;
                    result->args.push_back(convertExpr(arg_cursor));
                }
                if (result->args.empty()) appendSimpleParenArgFromCursor(result->args, cursor);
                if (result->args.empty()) appendSingleIdentifierCallArgFromCursor(result->args, cursor);
                if (result->args.empty()) appendIdentifierCallArgsFromCursor(result->args, cursor);
                prependLambdaCaptureArgs(token_callee, result->args, result->debug_loc);
                return wrapRecoveredCallWithTrailingBinary(result, cursorText(cursor));
            }
            failUnsupported(cursor, "S0 cannot resolve lambda/operator() receiver from cursor metadata");
        }
        if (spelling.rfind("operator", 0) == 0 && spelling != "operator()") {
            std::string op = spelling.substr(std::string("operator").size());
            op.erase(std::remove_if(op.begin(), op.end(), [](unsigned char c) {
                return std::isspace(c);
            }), op.end());
            std::vector<CXCursor> call_args;
            int arg_count = clang_Cursor_getNumArguments(cursor);
            for (int i = 0; i < arg_count; ++i) {
                call_args.push_back(clang_Cursor_getArgument(cursor, static_cast<unsigned>(i)));
            }
            if ((op == "+" || op == "-" || op == "*" || op == "&" || op == "|" || op == "^" ||
                 op == "<<" || op == ">>" || op == "==" || op == "!=" || op == "<" ||
                 op == "<=" || op == ">" || op == ">=")) {
                if (call_args.size() >= 2) {
                    auto lhs = convertExpr(call_args[0]);
                    auto rhs = convertExpr(call_args[1]);
                    markSignedViewIfCursorMentions(lhs, call_args[0]);
                    markSignedViewIfCursorMentions(rhs, call_args[1]);
                    return make_binary(op, lhs, rhs, convertType(type));
                }
                auto first = get_first_call_child();
                if (first && first->kind == ExprKind::FieldAccess &&
                first->field_name == spelling && first->struct_base &&
                children.size() >= 2) {
                auto rhs = convertExpr(children.back());
                markSignedViewIfCursorMentions(rhs, children.back());
                return make_binary(op, first->struct_base, rhs, convertType(type));
            }
            if (children.size() >= 3) {
                std::vector<std::pair<ExprPtr, CXCursor>> operands;
                for (auto& child : children) {
                    auto operand = unwrapSignedViewMemberAccess(convertExpr(child));
                    if (operand && !isOperatorOnlyExpr(operand)) {
                        operands.emplace_back(operand, child);
                    }
                    if (operands.size() >= 2) break;
                }
                if (operands.size() >= 2) {
                    auto lhs = operands[0].first;
                    auto rhs = operands[1].first;
                    markSignedViewIfCursorMentions(lhs, operands[0].second);
                    markSignedViewIfCursorMentions(rhs, operands[1].second);
                    return make_binary(op, lhs, rhs, convertType(type));
                }
            }
            if (children.size() >= 2 && first_child_spelling.rfind("operator", 0) != 0 &&
                first_child_spelling != spelling) {
                auto lhs = convertExpr(children[children.size() - 2]);
                auto rhs = convertExpr(children[children.size() - 1]);
                    markSignedViewIfCursorMentions(lhs, children[children.size() - 2]);
                    markSignedViewIfCursorMentions(rhs, children[children.size() - 1]);
                    return make_binary(op, lhs, rhs, convertType(type));
                }
            }
            if (op == "!" || op == "~" || op == "-" || op == "*") {
                if (call_args.size() == 1) {
                    return make_unary(op, convertExpr(call_args[0]), convertType(type));
                }
                auto first = get_first_call_child();
                if (first && first->kind == ExprKind::FieldAccess &&
                    first->field_name == spelling && first->struct_base) {
                    return make_unary(op, first->struct_base, convertType(type));
                }
                if (children.size() >= 2) {
                    return make_unary(op, convertExpr(children[1]), convertType(type));
                }
            }
        }
        if ((vul_call.kind == VulCallKind::OperatorCall || spelling == "operator()") &&
            !(get_first_call_child() && first_call_child->kind == ExprKind::FieldAccess)) {
            size_t receiver_index = children.size() >= 2 ? 1 : 0;
            auto receiver = children.size() > receiver_index ? convertExpr(children[receiver_index]) : nullptr;
            size_t arg_start = receiver_index + 1;
            std::vector<ExprPtr> args;
            for (size_t i = arg_start; i < children.size(); ++i) args.push_back(convertExpr(children[i]));
            std::string callee;
            if (children.size() > receiver_index) {
                callee = lambdaNameForReceiverCursor(children[receiver_index]);
            }
            if (callee.empty() && spelling != "operator()") callee = astBaseName(receiver);
            if (!callee.empty()) {
                int explicit_arg_count = clang_Cursor_getNumArguments(cursor);
                int receiver_arg_index = -1;
                for (int i = 0; i < explicit_arg_count; ++i) {
                    CXCursor arg_cursor = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
                    if (lambdaNameForReceiverCursor(arg_cursor) == callee) {
                        receiver_arg_index = i;
                        break;
                    }
                }
                if (explicit_arg_count > 0) {
                    args.clear();
                    for (int i = 0; i < explicit_arg_count; ++i) {
                        if (i == receiver_arg_index) continue;
                        CXCursor arg_cursor = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
                        if (isEmptyParenText(cursorText(arg_cursor))) continue;
                        auto arg = convertExpr(arg_cursor);
                        if (arg && !isOperatorOnlyExpr(arg)) args.push_back(std::move(arg));
                    }
                }
                if (args.empty()) appendSimpleParenArgFromCursor(args, cursor);
                if (args.empty()) appendSingleIdentifierCallArgFromCursor(args, cursor);
                if (args.empty()) appendIdentifierCallArgsFromCursor(args, cursor);
                auto result = std::make_shared<Expr>();
                result->kind = ExprKind::Call;
                result->callee = callee;
                result->type = convertType(type);
                result->literal_value = cursorText(cursor);
                result->args = std::move(args);
                if (known_lambda_names.count(callee)) {
                    prependLambdaCaptureArgs(callee, result->args, debugLocFromCursor(cursor));
                }
                return wrapRecoveredCallWithTrailingBinary(result, cursorText(cursor));
            }
        }
        if (spelling == "array" && children.size() == 1) {
            return convertExpr(children.back());
        }
        if (vul_call.kind == VulCallKind::SignedView && !children.empty()) {
            ExprPtr receiver;
            if (vul_call.has_receiver) {
                receiver = convertExpr(vul_call.receiver_cursor);
            } else {
                receiver = convertExpr(children.front());
            }
            receiver = unwrapSignedViewMemberAccess(receiver);
            std::vector<ExprPtr> args;
            if (receiver) args.push_back(receiver);
            return makeSurfaceCall("sint", convertType(type), std::move(args), debugLocFromCursor(cursor));
        }
        if (vul_call.kind == VulCallKind::At) {
            auto result = makeSurfaceCall("at", convertType(type), {}, debugLocFromCursor(cursor));
            std::vector<int> range_args = vul_call.template_values;
            if (range_args.size() != 2) {
                range_args = templateIntArgsFromTokens(cursor, "at");
            }
            if (range_args.size() == 1) {
                range_args.push_back(range_args.front());
            }
            if (range_args.size() != 2) {
                range_args = staticRangeArgsFromType(type);
            }
            if (range_args.size() != 2) {
                return result;
            }
            int hi = range_args[0];
            int lo = range_args[1];
            ExprPtr receiver;
            if (vul_call.has_receiver) {
                receiver = convertExpr(vul_call.receiver_cursor);
            } else if (!children.empty()) {
                receiver = convertExpr(children.front());
                if (receiver && receiver->kind == ExprKind::FieldAccess &&
                    receiver->field_name == "at" && receiver->struct_base) {
                    receiver = receiver->struct_base;
                }
            }
            if (!receiver) {
                return result;
            }
            result->hi = hi;
            result->lo = lo;
            result->args.push_back(receiver);
            return result;
        }
        if (vul_call.kind == VulCallKind::To) {
            TypeInfo target = convertType(type);
            if (!isStandardIntegerToTarget(target)) {
                return makeSurfaceCall("to", target, {}, debugLocFromCursor(cursor));
            }
            ExprPtr receiver;
            if (vul_call.has_receiver) {
                receiver = convertExpr(vul_call.receiver_cursor);
            } else if (!children.empty()) {
                receiver = convertExpr(children.front());
                if (receiver && receiver->kind == ExprKind::FieldAccess &&
                    receiver->field_name == "to" && receiver->struct_base) {
                    receiver = receiver->struct_base;
                }
            }
            if (!receiver) {
                return makeSurfaceCall("to", target, {}, debugLocFromCursor(cursor));
            }
            return makeSurfaceCall("to", hardwareIntegerTypeForStandardTarget(target),
                                   {receiver}, debugLocFromCursor(cursor));
        }
        if ((spelling.empty() || spelling == "operator()") && children.size() >= 2) {
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess &&
                receiver->field_name == "operator()") {
                std::vector<ExprPtr> args;
                for (size_t i = 1; i < children.size(); ++i) args.push_back(convertExpr(children[i]));
                auto result = makeSurfaceCall("operator()", convertType(type), {}, debugLocFromCursor(cursor));
                if (receiver->struct_base) result->args.push_back(receiver->struct_base);
                for (auto& a : args) result->args.push_back(a);
                return result;
            }
        }
        if (spelling == "operator[]" && children.size() >= 2) {
            ExprPtr base;
            ExprPtr idx;
            int arg_count = clang_Cursor_getNumArguments(cursor);
            if (arg_count >= 2) {
                base = convertExpr(clang_Cursor_getArgument(cursor, 0));
                idx = convertExpr(clang_Cursor_getArgument(cursor, 1));
            } else {
                base = convertExpr(children[children.size() - 2]);
                idx = convertExpr(children[children.size() - 1]);
            }
            while (idx && idx->kind == ExprKind::Cast && idx->cast_expr &&
                   idx->cast_expr->kind == ExprKind::Literal) {
                idx = idx->cast_expr;
            }
            if (base && base->kind == ExprKind::ArrayAccess && !base->type.is_array) {
                if (sameLiteralExpr(base->index, idx)) return base;
                return make_array_access(base->array_base, idx, convertType(type));
            }
            if ((base && base->kind == ExprKind::VarRef && base->var_name == "operator[]") ||
                (base && astBaseName(base).empty())) {
                failUnsupported(cursor, "S0 cannot resolve operator[] base from cursor metadata");
            }
            TypeInfo elem_type = convertType(type);
            if (base && base->type.is_array && !base->type.array_dims.empty()) {
                elem_type = base->type;
                elem_type.array_dims.erase(elem_type.array_dims.begin());
                elem_type.is_array = !elem_type.array_dims.empty();
                elem_type.array_size = elem_type.is_array ? elem_type.array_dims.front() : 0;
            }
            return make_array_access(base, idx, elem_type);
        }
        if (vul_call.kind == VulCallKind::Pick ||
            vul_call.kind == VulCallKind::RangeAt || vul_call.kind == VulCallKind::BitAt ||
            spelling.find("range_at") != std::string::npos ||
            spelling.find("bit_at") != std::string::npos) {
            bool is_bit_at = vul_call.kind == VulCallKind::BitAt ||
                (vul_call.kind == VulCallKind::Pick && !vul_call.template_value.has_value()) ||
                spelling.find("bit_at") != std::string::npos;
            auto result = makeSurfaceCall(is_bit_at ? "bit_at" : "range_at",
                                          is_bit_at ? make_hw_type("bool", 1, false) : convertType(type),
                                          {}, debugLocFromCursor(cursor));
            if (vul_call.kind == VulCallKind::Pick && !vul_call.template_value.has_value()) {
                auto pick_args = templateIntArgsFromTokens(cursor, "pick");
                if (!pick_args.empty()) {
                    vul_call.template_value = pick_args.front();
                    is_bit_at = false;
                    result->callee = "pick";
                    result->type = convertType(type);
                }
            }
            if (!is_bit_at && vul_call.template_value.has_value()) {
                result->to_width = *vul_call.template_value;
            }
            if (!is_bit_at && result->type.width <= 0 && vul_call.template_value.has_value()) {
                result->type = make_hw_type("Int", *vul_call.template_value, false);
            }
            if (vul_call.has_receiver) {
                result->args.push_back(convertExpr(vul_call.receiver_cursor));
            } else if (!children.empty()) {
                auto receiver = convertExpr(children.front());
                if (receiver && receiver->kind == ExprKind::FieldAccess && receiver->struct_base) {
                    result->args.push_back(receiver->struct_base);
                } else if (receiver) {
                    result->args.push_back(receiver);
                }
            }
            if (!vul_call.normal_arg_cursors.empty()) {
                for (auto arg_cursor : vul_call.normal_arg_cursors) {
                    result->args.push_back(convertExpr(arg_cursor));
                }
            } else {
                for (size_t i = 1; i < children.size(); ++i) {
                    result->args.push_back(convertExpr(children[i]));
                }
            }
            return result;
        }
        if (spelling == "Int" && !children.empty()) {
            auto inner = convertExpr(children.back());
            TypeInfo target = convertType(type);
            if (target.width > 0 && (!inner || inner->type.width != target.width)) {
                auto cast = std::make_shared<Expr>();
                cast->kind = ExprKind::Cast;
                cast->cast_type = target;
                cast->type = target;
                cast->cast_expr = inner;
                return cast;
            }
            return inner;
        }
        if (vul_call.kind == VulCallKind::Cat && children.size() >= 2) {
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess &&
                (receiver->field_name == "cat" || receiver->field_name == "concat")) {
                std::vector<ExprPtr> args;
                args.push_back(receiver->struct_base);
                for (size_t i = 1; i < children.size(); ++i) args.push_back(convertExpr(children[i]));
                return makeSurfaceCall(receiver->field_name, convertType(type), std::move(args),
                                       debugLocFromCursor(cursor));
            }
        }
        if (vul_call.kind == VulCallKind::Cat && children.size() > 1) {
            std::vector<ExprPtr> args;
            size_t start = 1;
            if (spelling == "Cat" || vul_call.method_name == "Cat") start = 1;
            for (size_t i = start; i < children.size(); ++i) {
                args.push_back(convertExpr(children[i]));
            }
            return makeSurfaceCall(vul_call.method_name.empty() ? "Cat" : vul_call.method_name,
                                   convertType(type), std::move(args), debugLocFromCursor(cursor));
        }
        if (vul_call.kind == VulCallKind::Repeat && !children.empty()) {
            int count = vul_call.template_value.value_or(templateArgInt(cursor, 0, 0));
            if (count <= 0) count = templateArgIntFromTokens(cursor, "repeat", -1);
            if (children.size() >= 1) {
                auto receiver = convertExpr(children.front());
                if (receiver && receiver->kind == ExprKind::FieldAccess &&
                    receiver->field_name == "repeat") {
                    auto result = makeSurfaceCall("repeat", convertType(type),
                                                  {receiver->struct_base}, debugLocFromCursor(cursor));
                    result->times = count;
                    return result;
                }
            }
            if (count <= 0 && children.size() >= 3) {
                auto c = convertExpr(children[1]);
                if (c && c->kind == ExprKind::Literal) {
                    try { count = std::stoi(c->literal_value, nullptr, 0); } catch (...) {}
                }
                auto result = makeSurfaceCall("repeat", convertType(type),
                                              {convertExpr(children[2])}, debugLocFromCursor(cursor));
                result->times = count;
                return result;
            }
            auto result = makeSurfaceCall("repeat", convertType(type),
                                          {convertExpr(children.back())}, debugLocFromCursor(cursor));
            result->times = count;
            return result;
        }
        if (vul_call.kind == VulCallKind::ReduceOr && !children.empty()) {
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess && receiver->field_name == "reduce_or") {
                return makeSurfaceCall("reduce_or", convertType(type), {receiver->struct_base},
                                       debugLocFromCursor(cursor));
            }
            return makeSurfaceCall("reduce_or", convertType(type), {convertExpr(children.back())},
                                   debugLocFromCursor(cursor));
        }
        if (vul_call.kind == VulCallKind::ReduceAnd && !children.empty()) {
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess && receiver->field_name == "reduce_and") {
                return makeSurfaceCall("reduce_and", convertType(type), {receiver->struct_base},
                                       debugLocFromCursor(cursor));
            }
            return makeSurfaceCall("reduce_and", convertType(type), {convertExpr(children.back())},
                                   debugLocFromCursor(cursor));
        }
        if (vul_call.kind == VulCallKind::ReduceXor && !children.empty()) {
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess && receiver->field_name == "reduce_xor") {
                return makeSurfaceCall("reduce_xor", convertType(type), {receiver->struct_base},
                                       debugLocFromCursor(cursor));
            }
            return makeSurfaceCall("reduce_xor", convertType(type), {convertExpr(children.back())},
                                   debugLocFromCursor(cursor));
        }
        if (spelling.rfind("operator", 0) == 0) {
            std::string op = spelling.substr(std::string("operator").size());
            op.erase(std::remove_if(op.begin(), op.end(), [](unsigned char c) {
                return std::isspace(c);
            }), op.end());
            if ((op == "+" || op == "-" || op == "*" || op == "&" || op == "|" ||
                 op == "^" || op == "<<" || op == ">>" || op == "==" ||
                 op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") &&
                children.size() >= 2) {
                size_t lhs_index = children.size() - 2;
                size_t rhs_index = children.size() - 1;
                if (children.size() == 3 &&
                    cxToStr(clang_getCursorSpelling(children[1])).rfind("operator", 0) == 0) {
                    lhs_index = 0;
                    rhs_index = 2;
                }
                return make_binary(op,
                                   convertExpr(children[lhs_index]),
                                   convertExpr(children[rhs_index]),
                                   convertType(type));
            }
            if ((op == "!" || op == "~" || op == "-" || op == "*") && children.size() >= 2) {
                return make_unary(op, convertExpr(children.back()), convertType(type));
            }
            if (spelling.find("operator ") != std::string::npos && !children.empty()) {
                auto inner = convertExpr(children.back());
                TypeInfo target = convertType(type);
                if (target.width > 0 && inner && inner->type.width != target.width) {
                    auto cast = std::make_shared<Expr>();
                    cast->kind = ExprKind::Cast;
                    cast->cast_type = target;
                    cast->type = target;
                    cast->cast_expr = inner;
                    return cast;
                }
                return inner;
            }
        }
        if (spelling_was_empty && children.size() >= 2) {
            auto lhs_cursor = children[children.size() - 2];
            auto rhs_cursor = children[children.size() - 1];
            CXSourceRange range = clang_getCursorExtent(cursor);
            CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
            CXToken* tokens = nullptr;
            unsigned numTokens = 0;
            clang_tokenize(tu, range, &tokens, &numTokens);

            CXSourceLocation lhsEnd = clang_getRangeEnd(clang_getCursorExtent(lhs_cursor));
            CXSourceLocation rhsStart = clang_getRangeStart(clang_getCursorExtent(rhs_cursor));
            unsigned lhsEndOffset = 0, rhsStartOffset = 0;
            CXFile f;
            unsigned line, col;
            clang_getSpellingLocation(lhsEnd, &f, &line, &col, &lhsEndOffset);
            clang_getSpellingLocation(rhsStart, &f, &line, &col, &rhsStartOffset);

            std::string op;
            for (unsigned i = 0; i < numTokens; ++i) {
                CXSourceLocation tokLoc = clang_getTokenLocation(tu, tokens[i]);
                unsigned tokOffset = 0;
                clang_getSpellingLocation(tokLoc, &f, &line, &col, &tokOffset);
                if (tokOffset >= lhsEndOffset && tokOffset < rhsStartOffset) {
                    op = cxToStr(clang_getTokenSpelling(tu, tokens[i]));
                    break;
                }
            }
            clang_disposeTokens(tu, tokens, numTokens);
            if (op == "+" || op == "-" || op == "*" || op == "&" || op == "|" ||
                op == "^" || op == "<<" || op == ">>" || op == "==" ||
                op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=") {
                return make_binary(op,
                                   convertExpr(lhs_cursor),
                                   convertExpr(rhs_cursor),
                                   convertType(type));
            }
        }
        if ((vul_call.kind == VulCallKind::ZExt ||
             vul_call.kind == VulCallKind::Trunc) && !children.empty()) {
            TypeInfo target = convertType(type);
            auto inner = convertExpr(children.back());
            if (target.width <= 0 && vul_call.template_value.has_value()) {
                target = make_hw_type("Int", *vul_call.template_value, false);
            }
            auto result = makeSurfaceCall(vul_call.kind == VulCallKind::Trunc ? "trunc" : "zext",
                                          target, {inner}, debugLocFromCursor(cursor));
            result->to_width = target.width;
            return result;
        }
        auto result = std::make_shared<Expr>();
        result->kind = ExprKind::Call;
        result->callee = spelling;
        result->type = typeForCallResult(type, spelling);
        size_t arg_start = 1;
        int receiver_arg_index = -1;
        if (spelling == "operator()") {
            result->callee.clear();
            for (size_t i = 0; i < children.size(); ++i) {
                if (firstSpellingDeep(children[i]) == "operator()") continue;
                std::string name = lambdaNameForReceiverCursor(children[i]);
                if (!name.empty() && name != "operator()") {
                    result->callee = name;
                    arg_start = i + 1;
                    break;
                }
            }
            int op_arg_count = clang_Cursor_getNumArguments(cursor);
            for (int i = 0; i < op_arg_count; ++i) {
                CXCursor arg_cursor = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
                std::string receiver_callee = lambdaNameForReceiverCursor(arg_cursor);
                if (!receiver_callee.empty()) {
                    if (result->callee.empty()) result->callee = receiver_callee;
                    if (receiver_callee == result->callee) {
                        receiver_arg_index = i;
                        break;
                    }
                }
            }
            if (result->callee.empty()) {
                result->callee = lambdaNameFromCallSourceText(cursor);
            }
            if (result->callee.empty()) {
                failUnsupported(cursor, "S0 cannot resolve operator() receiver from cursor metadata");
            }
        }
        if (!children.empty() && clang_getCursorKind(children.front()) == CXCursor_MemberRefExpr) {
            if (firstSpellingDeep(children.front()) != "operator()") {
                result->args.push_back(convertExpr(children.front()));
            }
        }
        if (!result->args.empty() && result->args.front() &&
            result->args.front()->kind == ExprKind::FieldAccess &&
            !result->args.front()->struct_base) {
            failUnsupported(cursor, "S0 cannot resolve member call receiver from cursor metadata");
        }
        // Use libclang's explicit argument API. Child[0] is not uniformly a
        // callee: for CXXConstructExpr it can be the first constructor
        // argument, and skipping it loses copy/list/direct initialization.
        int explicit_arg_count = clang_Cursor_getNumArguments(cursor);
        if (explicit_arg_count >= 0) {
            for (int i = 0; i < explicit_arg_count; ++i) {
                if (i == receiver_arg_index) continue;
                CXCursor arg_cursor = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
                if (isEmptyParenText(cursorText(arg_cursor))) continue;
                result->args.push_back(convertExpr(arg_cursor));
            }
            CXCursor referenced = clang_getCursorReferenced(cursor);
            const bool constructor_call =
                !clang_Cursor_isNull(referenced) &&
                clang_getCursorKind(referenced) == CXCursor_Constructor;
            if (constructor_call && explicit_arg_count == 0) {
                for (const auto& child : children) {
                    CXCursorKind child_kind = clang_getCursorKind(child);
                    if (!clang_isExpression(child_kind)) continue;
                    auto argument = convertExpr(child);
                    if (argument && !isOperatorOnlyExpr(argument)) {
                        result->args.push_back(std::move(argument));
                    }
                }
            }
        } else {
            for (size_t i = arg_start; i < children.size(); ++i) {
                if (firstSpellingDeep(children[i]) == "operator()") continue;
                result->args.push_back(convertExpr(children[i]));
            }
        }
        if (known_lambda_names.count(result->callee)) {
            result->literal_value = cursorText(cursor);
            if (result->args.empty()) appendSimpleParenArgFromCursor(result->args, cursor);
            if (result->args.empty()) appendSingleIdentifierCallArgFromCursor(result->args, cursor);
            if (result->args.empty()) appendIdentifierCallArgsFromCursor(result->args, cursor);
            prependLambdaCaptureArgs(result->callee, result->args, debugLocFromCursor(cursor));
            return wrapRecoveredCallWithTrailingBinary(result, cursorText(cursor));
        }
        return result;
    }

    case CXCursor_CStyleCastExpr:
    case CXCursor_CXXStaticCastExpr:
    case CXCursor_CXXFunctionalCastExpr: {
        auto result = std::make_shared<Expr>();
        result->kind = ExprKind::Cast;
        result->cast_type = convertType(type);
        result->type = result->cast_type;
        if (!children.empty()) {
            result->cast_expr = convertExpr(children.back());
        }
        return result;
    }

    case CXCursor_ParenExpr: {
        if (!children.empty()) return convertExpr(children[0]);
        break;
    }

    default: {
        // Try to recurse into first child for implicit casts
        if (!children.empty()) {
            std::string lambda_callee;
            std::size_t lambda_receiver_index = children.size();
            for (std::size_t i = 0; i < children.size(); ++i) {
                lambda_callee = lambdaNameForReceiverCursor(children[i]);
                if (!lambda_callee.empty()) {
                    lambda_receiver_index = i;
                    break;
                }
            }
            if (!lambda_callee.empty()) {
                auto recovered = std::make_shared<Expr>();
                recovered->kind = ExprKind::Call;
                recovered->callee = lambda_callee;
                recovered->type = convertType(type);
                recovered->literal_value = cursorText(cursor);
                for (std::size_t i = lambda_receiver_index + 1; i < children.size(); ++i) {
                    if (!clang_isExpression(clang_getCursorKind(children[i]))) continue;
                    if (firstSpellingDeep(children[i]).rfind("operator", 0) == 0) continue;
                    auto arg = convertExpr(children[i]);
                    if (arg && !isOperatorOnlyExpr(arg)) recovered->args.push_back(std::move(arg));
                }
                prependLambdaCaptureArgs(lambda_callee, recovered->args, debugLocFromCursor(cursor));
                return wrapRecoveredCallWithTrailingBinary(recovered, cursorText(cursor));
            }
            if (firstSpellingDeep(children.front()).rfind("operator", 0) == 0) {
                auto converted = convertExpr(children.front());
                TypeInfo target_type = convertType(type);
                if (converted && converted->kind == ExprKind::FieldAccess &&
                    converted->field_name.rfind("operator", 0) == 0 &&
                    converted->struct_base) {
                    converted = converted->struct_base;
                }
                if (converted && target_type.width > 0 &&
                    !target_type.is_array && target_type.struct_name.empty() &&
                    !converted->type.is_array &&
                    converted->type.struct_name.empty()) {
                    if (converted->type.width <= 0) {
                        converted->type = target_type;
                    } else if (converted->type.width != target_type.width ||
                               converted->type.is_signed != target_type.is_signed ||
                               converted->type.hw_kind != target_type.hw_kind) {
                        auto cast = std::make_shared<Expr>();
                        cast->kind = ExprKind::Cast;
                        cast->cast_type = target_type;
                        cast->type = target_type;
                        cast->cast_expr = converted;
                        return cast;
                    }
                }
                if (converted && !isOperatorOnlyExpr(converted)) return converted;
            }
            if (children.size() > 1 && firstSpellingDeep(children.front()).rfind("operator", 0) == 0) {
                for (size_t i = 1; i < children.size(); ++i) {
                    auto candidate = convertExpr(children[i]);
                    if (!isOperatorOnlyExpr(candidate)) return candidate;
                }
            }
            auto candidate = convertExpr(children[0]);
            if (!isOperatorOnlyExpr(candidate)) {
                if (kind == CXCursor_UnexposedExpr) {
                    TypeInfo target_type = convertType(type);
                    if (candidate &&
                        candidate->intrinsic == IntrinsicKind::DynamicRangeAt &&
                        candidate->type.width <= 0 &&
                        target_type.width > 0) {
                        candidate->type = target_type;
                        return candidate;
                    }
                    return makeImplicitBuiltinCast(candidate, target_type);
                }
                return candidate;
            }
            std::string callee = lambdaNameFromCallSourceText(cursor);
            if (!callee.empty()) {
                auto recovered = std::make_shared<Expr>();
                recovered->kind = ExprKind::Call;
                recovered->callee = callee;
                recovered->type = convertType(type);
                recovered->literal_value = cursorText(cursor);
                int arg_count = clang_Cursor_getNumArguments(cursor);
                int receiver_arg_index = -1;
                for (int i = 0; i < arg_count; ++i) {
                    CXCursor arg_cursor = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
                    if (lambdaNameForReceiverCursor(arg_cursor) == callee) {
                        receiver_arg_index = i;
                        break;
                    }
                }
                for (int i = 0; i < arg_count; ++i) {
                    if (i == receiver_arg_index) continue;
                    CXCursor arg_cursor = clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
                    if (isEmptyParenText(cursorText(arg_cursor))) continue;
                    auto arg = convertExpr(arg_cursor);
                    if (arg && !isOperatorOnlyExpr(arg)) recovered->args.push_back(std::move(arg));
                }
                if (recovered->args.empty()) {
                    appendSimpleParenArgFromCursor(recovered->args, cursor);
                }
                if (recovered->args.empty()) {
                    appendSingleIdentifierCallArgFromCursor(recovered->args, cursor);
                }
                if (recovered->args.empty()) {
                    appendIdentifierCallArgsFromCursor(recovered->args, cursor);
                }
            prependLambdaCaptureArgs(callee, recovered->args, debugLocFromCursor(cursor));
                return wrapRecoveredCallWithTrailingBinary(recovered, cursorText(cursor));
            }
            if (auto identifier = simpleParenthesizedIdentifier(cursorText(cursor))) {
                return make_var(*identifier, convertType(type));
            }
            if (isEmptyParenText(cursorText(cursor))) {
                return nullptr;
            }
            if (isParenIdentifierListText(cursorText(cursor))) {
                return nullptr;
            }
            if (auto int_construct = parseSimpleIntConstructText(cursorText(cursor))) {
                return int_construct;
            }
            failUnsupported(cursor, "S0 cannot resolve operator call receiver from cursor metadata");
        }
        break;
    }
    }

    failUnsupported(cursor,
        "Unsupported expression cursor kind=" +
        std::to_string(static_cast<int>(kind)) +
        " spelling='" + cxToStr(clang_getCursorSpelling(cursor)) + "'");
}

static ExprPtr convertExpr(CXCursor cursor) {
    return withDebugLoc(convertExprImpl(cursor), cursor);
}

static std::vector<StmtPtr> convertBlock(CXCursor cursor) {
    std::vector<StmtPtr> result;
    if (clang_getCursorKind(cursor) != CXCursor_CompoundStmt) {
        auto s = convertStmt(cursor);
        if (s) result.push_back(s);
        return result;
    }
    auto children = getChildren(cursor);
    for (auto& child : children) {
        auto s = convertStmt(child);
        if (s) result.push_back(s);
    }
    return result;
}

static bool isVulFixedIntType(const TypeInfo& type) {
    return type.width > 0 &&
           (type.hw_kind == "Int" ||
            type.hw_kind == "signed_view" || type.name.rfind("Int<", 0) == 0 ||
            type.name.rfind("IntSignedView<", 0) == 0);
}

static std::string canonicalStructLookupName(const TypeInfo& type) {
    std::string name = type.struct_name.empty() ? type.name : type.struct_name;
    if (name.rfind("struct ", 0) == 0) name = name.substr(7);
    if (name.rfind("class ", 0) == 0) name = name.substr(6);
    return name;
}

static const std::vector<StructFieldInfo>* activeStructFieldsFor(const TypeInfo& type) {
    if (!active_struct_fields) return nullptr;
    std::string canonical = canonicalStructLookupName(type);
    for (const auto& key : {type.struct_name, type.name, canonical,
                            std::string("struct ") + canonical}) {
        if (key.empty()) continue;
        auto it = active_struct_fields->find(key);
        if (it != active_struct_fields->end()) return &it->second;
    }
    return nullptr;
}

static bool activeStructHasUserConstructor(const TypeInfo& type) {
    if (!active_struct_constructors) return false;
    std::string canonical = canonicalStructLookupName(type);
    for (const auto& key : {type.struct_name, type.name, canonical,
                            std::string("struct ") + canonical}) {
        if (key.empty()) continue;
        auto it = active_struct_constructors->find(key);
        if (it != active_struct_constructors->end() && !it->second.empty()) return true;
    }
    return false;
}

static bool isAggregateInitTargetType(const TypeInfo& type) {
    return activeStructFieldsFor(type) != nullptr &&
           !activeStructHasUserConstructor(type);
}

static bool isAggregateInitCursor(CXCursor cursor) {
    CXCursorKind kind = clang_getCursorKind(cursor);
    return kind == CXCursor_InitListExpr;
}

static ExprPtr defaultValueForAggregateField(const TypeInfo& type) {
    if (type.name == "bool" || type.hw_kind == "bool") {
        return make_literal("false", TypeInfo{"bool", 1, false});
    }
    if (isVulFixedIntType(type) || type.width > 0) {
        return make_literal("0", type);
    }
    if (!type.struct_name.empty() || activeStructFieldsFor(type)) {
        auto call = std::make_shared<Expr>();
        call->kind = ExprKind::Call;
        call->callee = type.struct_name.empty() ? type.name : type.struct_name;
        call->type = type;
        return call;
    }
    return make_literal("0", type);
}

static ExprPtr convertInitArgExpr(CXCursor cursor, const TypeInfo& fallback_type) {
    CXCursor referenced = clang_getCursorReferenced(cursor);
    if (!clang_Cursor_isNull(referenced) &&
        clang_getCursorKind(referenced) == CXCursor_Constructor) {
        TypeInfo target_type = convertType(clang_getCursorType(cursor));
        auto children = getChildren(cursor);
        for (const auto& child : children) {
            CXCursorKind kind = clang_getCursorKind(child);
            if (kind == CXCursor_TypeRef || kind == CXCursor_TemplateRef ||
                kind == CXCursor_Constructor || kind == CXCursor_CXXMethod ||
                kind == CXCursor_FunctionDecl || isDesignatedInitFieldCursor(child)) {
                continue;
            }
            if (!clang_isExpression(kind)) continue;
            auto candidate = convertExpr(child);
            if (!candidate || isOperatorOnlyExpr(candidate)) continue;
            if (target_type.width > 0) {
                return makeImplicitBuiltinCast(candidate, target_type);
            }
            return candidate;
        }
        return defaultValueForAggregateField(
            fallback_type.name.empty() && fallback_type.struct_name.empty()
                ? target_type
                : fallback_type);
    }
    auto expr = convertExpr(cursor);
    if (expr) return expr;
    return defaultValueForAggregateField(
        fallback_type.name.empty() && fallback_type.struct_name.empty()
            ? convertType(clang_getCursorType(cursor))
            : fallback_type);
}

static bool isDesignatedInitFieldCursor(CXCursor cursor, int depth) {
    CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_MemberRef) return true;
    if (depth > 2) return false;
    auto children = getChildren(cursor);
    for (const auto& child : children) {
        if (isDesignatedInitFieldCursor(child, depth + 1)) return true;
    }
    return false;
}

static void collectInitArgExprs(CXCursor cursor, std::vector<ExprPtr>& out, int depth = 0) {
    if (depth > 3) return;
    auto cursor_kind = clang_getCursorKind(cursor);
    if (isDesignatedInitFieldCursor(cursor)) return;
    auto children = getChildren(cursor);
    if (children.empty()) {
        if (clang_isExpression(cursor_kind)) {
            out.push_back(convertInitArgExpr(cursor));
        }
        return;
    }

    if (cursor_kind == CXCursor_CallExpr || cursor_kind == CXCursor_InitListExpr) {
        for (auto& child : children) {
            auto kind = clang_getCursorKind(child);
            if (kind == CXCursor_TypeRef || kind == CXCursor_TemplateRef ||
                kind == CXCursor_Constructor || kind == CXCursor_CXXMethod ||
                kind == CXCursor_FunctionDecl ||
                isDesignatedInitFieldCursor(child)) {
                continue;
            }
            if (clang_isExpression(kind)) {
                out.push_back(convertInitArgExpr(child));
            }
        }
        return;
    }

    bool had_direct_expr = false;
    for (auto& child : children) {
        auto kind = clang_getCursorKind(child);
        if (kind == CXCursor_TypeRef || kind == CXCursor_TemplateRef ||
            kind == CXCursor_Constructor || kind == CXCursor_CXXMethod ||
            kind == CXCursor_FunctionDecl) {
            continue;
        }
        if (clang_isExpression(kind) &&
            (kind == CXCursor_DeclRefExpr || kind == CXCursor_MemberRefExpr ||
             kind == CXCursor_ArraySubscriptExpr || kind == CXCursor_CallExpr ||
             kind == CXCursor_IntegerLiteral || kind == CXCursor_CXXBoolLiteralExpr)) {
            out.push_back(convertInitArgExpr(child));
            had_direct_expr = true;
        }
    }
    if (had_direct_expr) return;
    for (auto& child : children) collectInitArgExprs(child, out, depth + 1);
}

static std::string designatedInitFieldName(CXCursor cursor, int depth = 0) {
    if (depth > 2) return {};
    CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_MemberRef) return cxToStr(clang_getCursorSpelling(cursor));
    auto children = getChildren(cursor);
    for (const auto& child : children) {
        auto name = designatedInitFieldName(child, depth + 1);
        if (!name.empty()) return name;
    }
    return {};
}

static ExprPtr designatedInitValueExpr(CXCursor cursor, int depth = 0) {
    if (depth > 4) return nullptr;
    auto children = getChildren(cursor);
    for (const auto& child : children) {
        CXCursorKind kind = clang_getCursorKind(child);
        if (kind == CXCursor_MemberRef) continue;
        if (isDesignatedInitFieldCursor(child)) {
            if (auto nested = designatedInitValueExpr(child, depth + 1)) return nested;
            continue;
        }
        if (clang_isExpression(kind)) {
            return convertInitArgExpr(child);
        }
        if (auto nested = designatedInitValueExpr(child, depth + 1)) return nested;
    }
    return nullptr;
}

static void collectDesignatedInitArgExprs(
    CXCursor cursor,
    std::vector<std::pair<std::string, ExprPtr>>& out,
    int depth = 0) {
    if (depth > 4) return;
    auto children = getChildren(cursor);
    CXCursorKind cursor_kind = clang_getCursorKind(cursor);
    if (cursor_kind == CXCursor_InitListExpr) {
        std::string pending_field;
        for (const auto& child : children) {
            if (isDesignatedInitFieldCursor(child)) {
                pending_field = designatedInitFieldName(child);
                if (auto value = designatedInitValueExpr(child)) {
                    out.push_back({pending_field, value});
                    pending_field.clear();
                }
                continue;
            }
            if (!pending_field.empty() && clang_isExpression(clang_getCursorKind(child))) {
                out.push_back({pending_field, convertInitArgExpr(child)});
                pending_field.clear();
            }
        }
        return;
    }
    for (const auto& child : children) {
        collectDesignatedInitArgExprs(child, out, depth + 1);
    }
}

static StmtPtr expandAggregateInitDecl(
    const StmtPtr& stmt,
    const std::vector<std::pair<std::string, ExprPtr>>& designated_args) {
    if (!stmt || stmt->kind != StmtKind::Decl) return nullptr;
    if (stmt->decl_init_args.empty() && designated_args.empty()) return nullptr;
    const auto* fields = activeStructFieldsFor(stmt->decl_type);
    if (!fields || fields->empty()) return nullptr;
    if (activeStructHasUserConstructor(stmt->decl_type)) return nullptr;
    if (stmt->decl_init_args.size() > fields->size()) return nullptr;
    for (const auto& field : *fields) {
        if (field.type.is_reference || field.type.is_pointer) return nullptr;
    }

    auto block = std::make_shared<Stmt>();
    block->kind = StmtKind::Block;
    block->synthetic_flatten_block = true;

    auto decl = std::make_shared<Stmt>(*stmt);
    decl->decl_init = std::nullopt;
    decl->decl_init_args.clear();
    decl->decl_default_constructed = false;
    block->block_stmts.push_back(decl);

    std::unordered_map<std::string, ExprPtr> designated_by_field;
    for (const auto& [field, value] : designated_args) {
        if (!field.empty() && value) designated_by_field[field] = value;
    }

    const bool has_designated_args = !designated_by_field.empty();
    for (std::size_t i = 0; i < fields->size(); ++i) {
        auto designated = designated_by_field.find((*fields)[i].name);
        ExprPtr value = designated != designated_by_field.end()
            ? designated->second
            : (!has_designated_args && i < stmt->decl_init_args.size()
                   ? stmt->decl_init_args[i]
                   : defaultValueForAggregateField((*fields)[i].type));
        if (!value) return nullptr;

        auto assign = std::make_shared<Stmt>();
        assign->kind = StmtKind::Assign;
        assign->debug_loc = stmt->debug_loc;
        assign->assign_target = make_field_access(
            make_var(stmt->decl_name, stmt->decl_type),
            (*fields)[i].name,
            (*fields)[i].type);
        assign->assign_value = value;
        block->block_stmts.push_back(assign);
    }
    return block;
}

static void collectIntegerInitValues(CXCursor cursor,
                                     std::vector<std::string>& out,
                                     int depth = 0) {
    if (depth > 8) return;
    CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_TypeRef || kind == CXCursor_TemplateRef ||
        kind == CXCursor_Constructor || kind == CXCursor_CXXMethod ||
        kind == CXCursor_FunctionDecl || isDesignatedInitFieldCursor(cursor)) {
        return;
    }
    auto children = getChildren(cursor);
    if (clang_isExpression(kind)) {
        if (kind == CXCursor_IntegerLiteral || kind == CXCursor_UnaryOperator ||
            kind == CXCursor_DeclRefExpr || children.empty()) {
            CXEvalResult eval = clang_Cursor_Evaluate(cursor);
            if (eval) {
                CXEvalResultKind eval_kind = clang_EvalResult_getKind(eval);
                if (eval_kind == CXEval_Int) {
                    out.push_back(std::to_string(clang_EvalResult_getAsLongLong(eval)));
                    clang_EvalResult_dispose(eval);
                    return;
                }
                clang_EvalResult_dispose(eval);
            }
        }
    }
    for (const auto& child : children) {
        collectIntegerInitValues(child, out, depth + 1);
    }
}

static StmtPtr convertStmtImpl(CXCursor cursor) {
    CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_LambdaExpr) return nullptr;
    auto children = getChildren(cursor);

    switch (kind) {
    case CXCursor_DeclStmt: {
        // May contain VarDecl children
        for (auto& child : children) {
            if (clang_getCursorKind(child) == CXCursor_VarDecl) {
                auto var_children = getChildren(child);
                if (!clang_equalCursors(findLambdaExpr(child), clang_getNullCursor())) {
                    return nullptr;
                }

                auto stmt = std::make_shared<Stmt>();
                stmt->kind = StmtKind::Decl;
                stmt->decl_name = cxToStr(clang_getCursorSpelling(child));
                stmt->decl_type = convertType(clang_getCursorType(child));
                if (clang_Cursor_getStorageClass(child) == CX_SC_Static) {
                    stmt->decl_type.is_static = true;
                }
                CXCursor init_expr = clang_getNullCursor();
                for (auto& vc : var_children) {
                    if (!clang_equalCursors(findLambdaExpr(vc), clang_getNullCursor())) continue;
                    if (clang_isExpression(clang_getCursorKind(vc))) {
                        init_expr = vc;
                    }
                }
                // libclang exposes an implicit CXXConstructExpr for declarations
                // such as `std::array<T, N> value;`.  It is not a source
                // initializer and must not become a surface `array()` call.
                if (!clang_Cursor_isNull(init_expr) && stmt->decl_type.is_array) {
                    std::string declaration_text = cursorText(child);
                    std::size_t name_pos = declaration_text.rfind(stmt->decl_name);
                    std::string tail = name_pos == std::string::npos
                        ? declaration_text
                        : declaration_text.substr(name_pos + stmt->decl_name.size());
                    const bool explicit_initializer =
                        tail.find('=') != std::string::npos ||
                        tail.find('{') != std::string::npos ||
                        tail.find('(') != std::string::npos;
                    if (!explicit_initializer) init_expr = clang_getNullCursor();
                }
                if (stmt->decl_type.is_static && stmt->decl_type.is_array &&
                    !clang_Cursor_isNull(init_expr)) {
                    collectIntegerInitValues(init_expr, stmt->decl_type.init_values);
                }
                stmt->decl_default_constructed =
                    clang_Cursor_isNull(init_expr) &&
                    isVulFixedIntType(stmt->decl_type);
                if (!clang_Cursor_isNull(init_expr)) {
                    stmt->decl_init = convertExpr(init_expr);
                    if (isAggregateInitTargetType(stmt->decl_type) &&
                        isAggregateInitCursor(init_expr)) {
                        collectInitArgExprs(init_expr, stmt->decl_init_args);
                        if (stmt->decl_type.is_array) {
                            std::size_t leaf_count = 1;
                            for (int dim : stmt->decl_type.array_dims) {
                                leaf_count *= static_cast<std::size_t>(dim);
                            }
                            TypeInfo scalar = stmt->decl_type;
                            scalar.is_array = false;
                            scalar.array_size = 0;
                            scalar.array_dims.clear();
                            while (stmt->decl_init_args.size() < leaf_count) {
                                stmt->decl_init_args.push_back(
                                    defaultValueForAggregateField(scalar));
                            }
                            // S1 represents explicit array aggregate
                            // initialization (including `= {}`) as a
                            // Construct with one value per flattened leaf.
                            stmt->decl_init = std::nullopt;
                        }
                    }
                    if (stmt->decl_type.is_array) {
                        const std::string declaration_text = cursorText(child);
                        const std::size_t name_pos =
                            declaration_text.rfind(stmt->decl_name);
                        const std::string tail = name_pos == std::string::npos
                            ? declaration_text
                            : declaration_text.substr(
                                  name_pos + stmt->decl_name.size());
                        std::string compact_tail = tail;
                        compact_tail.erase(std::remove_if(
                            compact_tail.begin(), compact_tail.end(),
                            [](unsigned char ch) { return std::isspace(ch); }),
                            compact_tail.end());
                        if (compact_tail.find("{}") != std::string::npos) {
                            std::size_t leaf_count = 1;
                            for (int dim : stmt->decl_type.array_dims) {
                                leaf_count *= static_cast<std::size_t>(dim);
                            }
                            TypeInfo scalar = stmt->decl_type;
                            scalar.is_array = false;
                            scalar.array_size = 0;
                            scalar.array_dims.clear();
                            while (stmt->decl_init_args.size() < leaf_count) {
                                stmt->decl_init_args.push_back(
                                    defaultValueForAggregateField(scalar));
                            }
                            stmt->decl_init = std::nullopt;
                        }
                    }
                }
                std::vector<std::pair<std::string, ExprPtr>> designated_args;
                if (!clang_Cursor_isNull(init_expr)) {
                    collectDesignatedInitArgExprs(init_expr, designated_args);
                }
                if (auto expanded = expandAggregateInitDecl(stmt, designated_args)) {
                    return expanded;
                }
                return stmt;
            }
        }
    return nullptr;
}

    case CXCursor_UnaryOperator: {
        auto unary = convertExpr(cursor);
        if (unary && (unary->op == "++" || unary->op == "--") &&
            !children.empty()) {
            auto target = convertExpr(children[0]);
            TypeInfo promoted = builtinIntegerPromotion(target ? target->type : TypeInfo{});
            ExprPtr lhs = target;
            ExprPtr one;
            TypeInfo result_type;
            if (target && isBuiltinIntegerType(target->type)) {
                lhs = makeImplicitBuiltinCast(target, promoted);
                one = make_literal("1", promoted);
                result_type = promoted;
            } else {
                lhs = target;
                TypeInfo one_type = target ? target->type : TypeInfo{"int", 32, true, true, "builtin"};
                one = make_literal("1", one_type);
                result_type = target ? target->type : one_type;
            }
            auto stmt = std::make_shared<Stmt>();
            stmt->kind = StmtKind::Assign;
            stmt->assign_target = target;
            stmt->assign_value = make_binary(unary->op == "++" ? "+" : "-",
                                             lhs,
                                             one,
                                             result_type);
            return stmt;
        }
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->expr_stmt = unary;
        return stmt;
    }

    case CXCursor_BinaryOperator:
    case CXCursor_CompoundAssignOperator: {
        // Check if it's an assignment
        if (children.size() >= 2) {
            std::string op = binaryOperatorFromCursor(cursor, children[0], children[1]);
            if (op == "=" || op == "+=" || op == "-=" || op == "*=" ||
                op == "/=" || op == "%=" || op == "&=" || op == "|=" ||
                op == "^=" || op == "<<=" || op == ">>=") {
                auto stmt = std::make_shared<Stmt>();
                stmt->kind = StmtKind::Assign;
                stmt->assign_target = convertExpr(children[0]);
                if (op == "=") {
                    stmt->assign_value = convertExpr(children[1]);
                } else {
                    // Compound: x += y 鈫?x = x + y
                    std::string base_op = op.substr(0, op.size() - 1);
                    stmt->assign_value = make_binary(base_op,
                        convertExpr(children[0]),
                        convertExpr(children[1]));
                }
                return stmt;
            }
        }
        // Not an assignment, treat as expression statement
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->expr_stmt = convertExpr(cursor);
        return stmt;
    }

    case CXCursor_CallExpr: {
        std::string spelling = cxToStr(clang_getCursorSpelling(cursor));
        if (spelling.empty() && !children.empty()) {
            std::string child_spelling = firstSpellingDeep(children[0]);
            if (child_spelling.rfind("operator", 0) == 0) spelling = child_spelling;
        }
        if (isOperatorSpelling(spelling, "=") && children.size() >= 2) {
            auto stmt = std::make_shared<Stmt>();
            stmt->kind = StmtKind::Assign;
            if (children.size() >= 3 && firstSpellingDeep(children[1]).rfind("operator=", 0) == 0) {
                stmt->assign_target = convertExpr(children[0]);
                stmt->assign_value = convertExpr(children[2]);
                return stmt;
            }
            int arg_count = clang_Cursor_getNumArguments(cursor);
            if (arg_count >= 2) {
                stmt->assign_target = convertExpr(clang_Cursor_getArgument(cursor, 0));
                stmt->assign_value = convertExpr(clang_Cursor_getArgument(cursor, 1));
                return stmt;
            }
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess &&
                receiver->field_name.rfind("operator", 0) == 0) {
                stmt->assign_target = receiver->struct_base;
            } else {
                size_t lhs_index = children.size() >= 2 ? children.size() - 2 : 0;
                stmt->assign_target = convertExpr(children[lhs_index]);
            }
            stmt->assign_value = convertExpr(children.back());
            return stmt;
        }
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->expr_stmt = convertExpr(cursor);
        return stmt;
    }

    case CXCursor_IfStmt: {
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::If;
        if (children.size() >= 2) {
            stmt->if_cond = convertExpr(children[0]);
            stmt->if_then = convertBlock(children[1]);
            if (children.size() >= 3) {
                stmt->if_else = convertBlock(children[2]);
            }
        }
        return stmt;
    }

    case CXCursor_ForStmt: {
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::For;
        // ForStmt children: init, cond, inc, body (some may be null/empty)
        if (children.size() >= 4) {
            stmt->for_init = convertStmt(children[0]);
            stmt->for_cond = convertExpr(children[1]);
            stmt->for_step = convertExpr(children[2]);
            stmt->for_body = convertBlock(children[3]);
        } else if (children.size() == 3) {
            // Might be missing init or inc
            stmt->for_init = convertStmt(children[0]);
            stmt->for_cond = convertExpr(children[1]);
            stmt->for_body = convertBlock(children[2]);
        }
        return stmt;
    }

    case CXCursor_WhileStmt: {
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::While;
        if (children.size() >= 2) {
            stmt->while_cond = convertExpr(children[0]);
            stmt->while_body = convertBlock(children[1]);
        }
        return stmt;
    }

    case CXCursor_DoStmt: {
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::DoWhile;
        if (children.size() >= 2) {
            stmt->while_body = convertBlock(children[0]);
            stmt->while_cond = convertExpr(children[1]);
        }
        return stmt;
    }

    case CXCursor_SwitchStmt: {
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::Switch;
        if (children.size() >= 2) {
            stmt->switch_expr = convertExpr(children[0]);
            // Body is a CompoundStmt with CaseStmt/DefaultStmt children
            auto body_children = getChildren(children[1]);
            CaseClause current;
            bool has_current = false;
            for (auto& bc : body_children) {
                CXCursorKind bk = clang_getCursorKind(bc);
                if (bk == CXCursor_CaseStmt) {
                    if (has_current) stmt->switch_cases.push_back(current);
                    current = CaseClause{};
                    auto case_children = getChildren(bc);
                    if (!case_children.empty()) {
                        current.value = convertExpr(case_children[0]);
                    }
                    for (size_t i = 1; i < case_children.size(); ++i) {
                        auto s = convertStmt(case_children[i]);
                        if (s) current.body.push_back(s);
                    }
                    has_current = true;
                } else if (bk == CXCursor_DefaultStmt) {
                    if (has_current) stmt->switch_cases.push_back(current);
                    current = CaseClause{};
                    current.value = std::nullopt;
                    auto def_children = getChildren(bc);
                    for (auto& dc : def_children) {
                        auto s = convertStmt(dc);
                        if (s) current.body.push_back(s);
                    }
                    has_current = true;
                } else {
                    if (has_current) {
                        auto s = convertStmt(bc);
                        if (s) current.body.push_back(s);
                    }
                }
            }
            if (has_current) stmt->switch_cases.push_back(current);
        }
        return stmt;
    }

    case CXCursor_BreakStmt: {
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::Break;
        return stmt;
    }

    case CXCursor_ContinueStmt: {
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::Continue;
        return stmt;
    }

    case CXCursor_ReturnStmt: {
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::Return;
        if (!children.empty()) {
            stmt->return_value = convertExpr(children[0]);
        }
        return stmt;
    }

    case CXCursor_CompoundStmt: {
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::Block;
        stmt->block_stmts = convertBlock(cursor);
        return stmt;
    }

    default: {
        if (!children.empty()) {
            auto nested = convertStmt(children[0]);
            if (nested && (nested->kind == StmtKind::Assign ||
                           nested->kind == StmtKind::Block ||
                           nested->kind == StmtKind::If ||
                           nested->kind == StmtKind::Return)) return nested;
        }
        // Try as expression statement
        if (clang_isExpression(kind)) {
            auto stmt = std::make_shared<Stmt>();
            stmt->kind = StmtKind::ExprStmt;
            stmt->expr_stmt = convertExpr(cursor);
            return stmt;
        }
        // Recurse into children for unknown statement kinds
        if (!children.empty()) {
            return convertStmt(children[0]);
        }
        return nullptr;
    }
    }
}

static StmtPtr convertStmt(CXCursor cursor) {
    return withDebugLoc(convertStmtImpl(cursor), cursor);
}

static std::unordered_map<std::string, TypeInfo>
collectRecoveredSymbolTypes(CXCursor cursor) {
    std::unordered_map<std::string, TypeInfo> result;
    clang_visitChildren(cursor, [](CXCursor child, CXCursor,
                                   CXClientData data) -> CXChildVisitResult {
        auto* result =
            static_cast<std::unordered_map<std::string, TypeInfo>*>(data);
        CXCursorKind kind = clang_getCursorKind(child);
        if (kind == CXCursor_VarDecl || kind == CXCursor_ParmDecl) {
            std::string name = cxToStr(clang_getCursorSpelling(child));
            if (!name.empty()) {
                result->emplace(name, convertType(clang_getCursorType(child)));
            }
        }
        return CXChildVisit_Recurse;
    }, &result);
    return result;
}

static std::vector<std::string> lambdaTemplateParamNamesFromText(const std::string& lambda_text) {
    std::vector<std::string> params;
    std::size_t capture_end = lambda_text.find(']');
    std::size_t template_begin = capture_end == std::string::npos
        ? std::string::npos
        : capture_end + 1;
    while (template_begin != std::string::npos &&
           template_begin < lambda_text.size() &&
           std::isspace(static_cast<unsigned char>(lambda_text[template_begin]))) {
        ++template_begin;
    }
    if (template_begin == std::string::npos ||
        template_begin >= lambda_text.size() ||
        lambda_text[template_begin] != '<') {
        return params;
    }

    int depth = 0;
    std::size_t template_end = std::string::npos;
    for (std::size_t i = template_begin; i < lambda_text.size(); ++i) {
        if (lambda_text[i] == '<') ++depth;
        if (lambda_text[i] == '>' && --depth == 0) {
            template_end = i;
            break;
        }
    }
    if (template_end == std::string::npos) return params;

    std::string params_text =
        lambda_text.substr(template_begin + 1, template_end - template_begin - 1);
    std::size_t begin = 0;
    int nested = 0;
    for (std::size_t i = 0; i <= params_text.size(); ++i) {
        char ch = i < params_text.size() ? params_text[i] : ',';
        if (ch == '<') ++nested;
        if (ch == '>') --nested;
        if (ch != ',' || nested != 0) continue;
        std::string part = params_text.substr(begin, i - begin);
        auto equal = part.find('=');
        if (equal != std::string::npos) part.resize(equal);
        while (!part.empty() &&
               std::isspace(static_cast<unsigned char>(part.back()))) {
            part.pop_back();
        }
        std::size_t name_end = part.size();
        std::size_t name_begin = name_end;
        while (name_begin > 0 &&
               (std::isalnum(static_cast<unsigned char>(part[name_begin - 1])) ||
                part[name_begin - 1] == '_')) {
            --name_begin;
        }
        std::string param_name = part.substr(name_begin, name_end - name_begin);
        if (!param_name.empty()) params.push_back(std::move(param_name));
        begin = i + 1;
    }
    return params;
}

static bool cursorTemplateArgsMatch(CXCursor cursor, const std::vector<int>& values) {
    int count = clang_Cursor_getNumTemplateArguments(cursor);
    if (count != static_cast<int>(values.size())) return false;
    for (int i = 0; i < count; ++i) {
        if (clang_Cursor_getTemplateArgumentKind(cursor, i) !=
            CXTemplateArgumentKind_Integral) {
            return false;
        }
        if (clang_Cursor_getTemplateArgumentValue(cursor, i) != values[i]) {
            return false;
        }
    }
    return true;
}

static CXCursor findSpecializedLambdaOperatorMethod(CXCursor lambda_cursor,
                                                    const std::vector<int>& values) {
    struct Ctx {
        const std::vector<int>* values;
        CXCursor found = clang_getNullCursor();
    } ctx{&values, clang_getNullCursor()};

    clang_visitChildren(lambda_cursor, [](CXCursor c, CXCursor,
                                          CXClientData data) -> CXChildVisitResult {
        auto* ctx = static_cast<Ctx*>(data);
        if (clang_getCursorKind(c) == CXCursor_CXXMethod &&
            cxToStr(clang_getCursorSpelling(c)) == "operator()" &&
            cursorTemplateArgsMatch(c, *ctx->values)) {
            ctx->found = c;
            return CXChildVisit_Break;
        }
        return CXChildVisit_Recurse;
    }, &ctx);
    return ctx.found;
}

static CXCursor referencedOperatorMethodInCursor(CXCursor cursor) {
    CXCursor referenced = clang_getCursorReferenced(cursor);
    if (!clang_Cursor_isNull(referenced) &&
        clang_getCursorKind(referenced) == CXCursor_CXXMethod &&
        cxToStr(clang_getCursorSpelling(referenced)) == "operator()") {
        return referenced;
    }
    for (auto& child : getChildren(cursor)) {
        CXCursor found = referencedOperatorMethodInCursor(child);
        if (!clang_Cursor_isNull(found)) return found;
    }
    return clang_getNullCursor();
}

static CXCursor primaryFunctionDeclForTemplate(CXCursor cursor) {
    if (clang_getCursorKind(cursor) != CXCursor_FunctionTemplate) return cursor;
    CXCursor found = clang_getNullCursor();
    clang_visitChildren(cursor, [](CXCursor child, CXCursor,
                                  CXClientData data) -> CXChildVisitResult {
        auto* found = static_cast<CXCursor*>(data);
        if (clang_getCursorKind(child) == CXCursor_FunctionDecl) {
            *found = child;
            return CXChildVisit_Break;
        }
        return CXChildVisit_Recurse;
    }, &found);
    return clang_Cursor_isNull(found) ? cursor : found;
}

static FunctionAST convertFunctionDecl(CXCursor cursor, const std::string& name) {
    auto saved_recovered_types = recovered_symbol_types;
    auto saved_const_int_values = global_const_int_values;
    recovered_symbol_types = collectRecoveredSymbolTypes(cursor);
    collectScopedConstInts(cursor, cursorFileName(cursor));
    FunctionAST func;
    func.name = name;
    func.return_type = convertType(clang_getCursorResultType(cursor));

    int numParams = clang_Cursor_getNumArguments(cursor);
    for (int i = 0; i < numParams; ++i) {
        CXCursor param = clang_Cursor_getArgument(cursor, i);
        ParamDecl pd = makeParamDeclFromCursor(param);
        func.params.push_back(pd);
    }

    auto func_children = getChildren(cursor);
    for (auto& child : func_children) {
        if (clang_getCursorKind(child) == CXCursor_CompoundStmt) {
            func.body = convertBlock(child);
            break;
        }
    }

    for (auto& p : func.params) {
        p.direction = inferParamDirection(p);
        p.is_output = p.direction != ParamDirection::Input;
    }

    recovered_symbol_types = std::move(saved_recovered_types);
    global_const_int_values = std::move(saved_const_int_values);
    return func;
}

static std::shared_ptr<FunctionAST> convertLambdaExpr(
    CXCursor lambda_cursor,
    const std::string& name,
    const std::vector<int>& template_values = {},
    bool include_template_params = true,
    bool register_operator_name = true,
    CXCursor preferred_operator_method = clang_getNullCursor()) {
    auto saved_recovered_types = recovered_symbol_types;
    auto saved_const_int_values = global_const_int_values;
    auto saved_template_int_values = current_template_int_values;
    recovered_symbol_types = collectRecoveredSymbolTypes(lambda_cursor);
    collectScopedConstInts(lambda_cursor, cursorFileName(lambda_cursor));
    const std::string lambda_text = cursorText(lambda_cursor);
    auto template_param_names = lambdaTemplateParamNamesFromText(lambda_text);
    if (!template_values.empty()) {
        if (template_values.size() != template_param_names.size()) {
            failUnsupported(lambda_cursor,
                            "S0 cannot specialize generic lambda with mismatched template argument count");
        }
        for (std::size_t i = 0; i < template_values.size(); ++i) {
            current_template_int_values[template_param_names[i]] = template_values[i];
        }
    }
    auto fn = std::make_shared<FunctionAST>();
    fn->name = name;
    known_lambda_names.insert(name);
    if (register_operator_name) registerLambdaOperatorName(lambda_cursor, name);
    fn->return_type = TypeInfo{"auto", 0, false};

    auto captures = collectLambdaCaptures(lambda_cursor);
    lambda_captures_by_name[name] = captures;
    for (const auto& capture : captures) {
        appendCaptureParam(*fn, capture);
    }

    auto find_template_param_type = [&](const std::string& param_name) {
        TypeInfo found;
        auto visit = [&](CXCursor cursor, auto& self) -> void {
            if (!found.name.empty()) return;
            if (clang_getCursorKind(cursor) == CXCursor_DeclRefExpr &&
                cxToStr(clang_getCursorSpelling(cursor)) == param_name) {
                found = convertType(clang_getCursorType(cursor));
                return;
            }
            for (auto& child : getChildren(cursor)) self(child, self);
        };
        visit(lambda_cursor, visit);
        if (found.name.empty()) {
            found = TypeInfo{"uint32_t", 32, false, true, "builtin"};
        }
        return found;
    };

    if (include_template_params) {
        for (const auto& param_name : template_param_names) {
            if (!param_name.empty() && !hasParamNamed(*fn, param_name)) {
                ParamDecl param;
                param.name = param_name;
                param.type = find_template_param_type(param_name);
                param.passing = ParamPassingKind::Value;
                param.direction = ParamDirection::Input;
                param.is_const = true;
                fn->params.push_back(std::move(param));
            }
        }
    }

    auto add_explicit_param = [&](CXCursor param_cursor) {
        ParamDecl param = makeParamDeclFromCursor(param_cursor);
        fn->params.push_back(std::move(param));
    };

    auto collect_lambda_method = [&](CXCursor method) {
        TypeInfo method_return = convertType(clang_getCursorResultType(method));
        if (!method_return.name.empty() || method_return.width > 0) {
            fn->return_type = method_return;
        }
        for (auto& mc : getChildren(method)) {
            auto mk = clang_getCursorKind(mc);
            if (mk == CXCursor_ParmDecl) {
                add_explicit_param(mc);
            } else if (mk == CXCursor_CompoundStmt && fn->body.empty()) {
                fn->body = convertBlock(mc);
            }
        }
    };

    CXCursor selected_specialization = preferred_operator_method;
    if (clang_Cursor_isNull(selected_specialization) && !template_values.empty()) {
        selected_specialization =
            findSpecializedLambdaOperatorMethod(lambda_cursor, template_values);
    }
    if (!clang_Cursor_isNull(selected_specialization)) {
        collect_lambda_method(selected_specialization);
    } else {
        auto lambda_children = getChildren(lambda_cursor);
        for (auto& c : lambda_children) {
            auto kind = clang_getCursorKind(c);
            if (kind == CXCursor_CXXMethod &&
                cxToStr(clang_getCursorSpelling(c)) == "operator()") {
                collect_lambda_method(c);
            } else if (kind == CXCursor_ParmDecl) {
                add_explicit_param(c);
            } else if (kind == CXCursor_CompoundStmt && fn->body.empty()) {
                fn->body = convertBlock(c);
            }
        }
    }

    if (fn->body.empty()) {
        struct MethodSearchCtx {
            decltype(collect_lambda_method)* collect;
            bool found = false;
        } method_ctx{&collect_lambda_method, false};
        clang_visitChildren(lambda_cursor, [](CXCursor c, CXCursor, CXClientData data) {
            auto* ctx = static_cast<MethodSearchCtx*>(data);
            if (clang_getCursorKind(c) == CXCursor_CXXMethod &&
                cxToStr(clang_getCursorSpelling(c)) == "operator()") {
                (*ctx->collect)(c);
                ctx->found = true;
                return CXChildVisit_Break;
            }
            return CXChildVisit_Continue;
        }, &method_ctx);
    }

    collectLocalLambdas(lambda_cursor, *fn);

    for (auto& s : fn->body) {
        if (s && s->kind == StmtKind::Return && s->return_value.has_value()) {
            const TypeInfo& return_type = s->return_value.value()->type;
            if (!return_type.name.empty() && return_type.width > 0) {
                fn->return_type = return_type;
            }
            break;
        }
    }
    if (fn->return_type.name == "auto" && fn->return_type.width == 0) {
        fn->return_type = TypeInfo{"void", 0, false};
    }
    lambda_param_count_by_name[name] = fn->params.size();
    function_return_types_by_name[name] = fn->return_type;
    recovered_symbol_types = std::move(saved_recovered_types);
    global_const_int_values = std::move(saved_const_int_values);
    current_template_int_values = std::move(saved_template_int_values);
    return fn;
}

static void collectLocalLambdas(CXCursor cursor, FunctionAST& func) {
    for (auto& child : getChildren(cursor)) {
        if (clang_getCursorKind(child) == CXCursor_VarDecl) {
            std::string source_name = cxToStr(clang_getCursorSpelling(child));
            CXCursor lambda_cursor = findLambdaExpr(child);
            if (!clang_equalCursors(lambda_cursor, clang_getNullCursor())) {
                std::string lambda_name = ensureLambdaNameForVar(child, source_name);
                auto template_params =
                    lambdaTemplateParamNamesFromText(cursorText(lambda_cursor));
                if (!template_params.empty()) {
                    auto specs =
                        lambda_template_specialization_args_by_name.find(lambda_name);
                    if (specs == lambda_template_specialization_args_by_name.end() ||
                        specs->second.empty()) {
                        continue;
                    }
                    for (const auto& args : specs->second) {
                        std::string specialized_name =
                            ensureLambdaTemplateSpecializationName(lambda_name, args);
                        CXCursor preferred_method = clang_getNullCursor();
                        auto method_it = lambda_template_specialization_method_by_key.find(
                            lambdaTemplateSpecializationKey(lambda_name, args));
                        if (method_it != lambda_template_specialization_method_by_key.end()) {
                            preferred_method = method_it->second;
                        }
                        auto lambda = convertLambdaExpr(lambda_cursor,
                                                        specialized_name,
                                                        args,
                                                        false,
                                                        false,
                                                        preferred_method);
                        if (lambda) func.lambdas[specialized_name] = lambda;
                    }
                } else {
                    auto lambda = convertLambdaExpr(lambda_cursor, lambda_name);
                    if (lambda) func.lambdas[lambda_name] = lambda;
                }
                continue;
            }
        }
        collectLocalLambdas(child, func);
    }
}

static void appendPendingFunctionTemplateSpecializations(FunctionAST& root) {
    for (std::size_t i = 0; i < pending_function_template_specializations.size(); ++i) {
        const auto spec = pending_function_template_specializations[i];
        if (!processed_function_template_specializations.insert(spec.internal_name).second) {
            continue;
        }
        auto template_it = source_function_templates_by_name.find(spec.source_name);
        if (template_it == source_function_templates_by_name.end()) continue;
        const auto& info = template_it->second;
        if (spec.args.size() != info.template_params.size()) continue;

        auto saved_template_int_values = current_template_int_values;
        for (std::size_t arg_index = 0; arg_index < spec.args.size(); ++arg_index) {
            current_template_int_values[info.template_params[arg_index]] =
                spec.args[arg_index];
        }

        CXCursor function_cursor = primaryFunctionDeclForTemplate(info.function_decl);
        auto helper = std::make_shared<FunctionAST>(
            convertFunctionDecl(function_cursor, spec.internal_name));
        if (helper->params.empty() &&
            clang_getCursorKind(function_cursor) == CXCursor_FunctionTemplate) {
            helper->params = parseFunctionParamsFromTemplateText(
                cursorText(function_cursor, true), spec.source_name);
        }
        helper->struct_fields = root.struct_fields;
        helper->struct_constructors = root.struct_constructors;
        {
            auto saved_const_int_values = global_const_int_values;
            collectScopedConstInts(function_cursor, cursorFileName(function_cursor));
            collectLocalLambdas(function_cursor, *helper);
            global_const_int_values = std::move(saved_const_int_values);
        }
        current_template_int_values = std::move(saved_template_int_values);
        root.helpers.push_back(std::move(helper));
    }
}

static std::unordered_map<std::string, long long>
templateBindingsForSpecializedFunction(CXCursor cursor) {
    std::unordered_map<std::string, long long> bindings;
    if (clang_Cursor_isNull(clang_getSpecializedCursorTemplate(cursor))) {
        return bindings;
    }
    std::string source_name = cxToStr(clang_getCursorSpelling(cursor));
    auto template_it = source_function_templates_by_name.find(source_name);
    if (template_it == source_function_templates_by_name.end()) return bindings;
    int count = clang_Cursor_getNumTemplateArguments(cursor);
    if (count != static_cast<int>(template_it->second.template_params.size())) {
        return bindings;
    }
    for (int i = 0; i < count; ++i) {
        if (clang_Cursor_getTemplateArgumentKind(cursor, i) !=
            CXTemplateArgumentKind_Integral) {
            return {};
        }
        bindings[template_it->second.template_params[i]] =
            clang_Cursor_getTemplateArgumentValue(cursor, i);
    }
    return bindings;
}

static void collectFunctionLambdas(FunctionAST& fn,
                                   std::unordered_map<std::string, FunctionAST*>& out) {
    for (auto& [name, lambda] : fn.lambdas) {
        if (!lambda) continue;
        out[name] = lambda.get();
        collectFunctionLambdas(*lambda, out);
    }
    for (auto& helper : fn.helpers) {
        if (helper) collectFunctionLambdas(*helper, out);
    }
}

static void collectCalledLambdasExpr(const ExprPtr& expr,
                                     std::unordered_set<std::string>& out);
static void collectLambdaCallArgVarsExpr(const ExprPtr& expr,
                                         std::unordered_set<std::string>& out);

static void collectCalledLambdasStmt(const StmtPtr& stmt,
                                     std::unordered_set<std::string>& out) {
    if (!stmt) return;
    collectCalledLambdasExpr(stmt->assign_target, out);
    collectCalledLambdasExpr(stmt->assign_value, out);
    if (stmt->decl_init) collectCalledLambdasExpr(*stmt->decl_init, out);
    for (const auto& arg : stmt->decl_init_args) collectCalledLambdasExpr(arg, out);
    collectCalledLambdasExpr(stmt->if_cond, out);
    if (stmt->for_init) collectCalledLambdasStmt(stmt->for_init, out);
    collectCalledLambdasExpr(stmt->for_cond, out);
    collectCalledLambdasExpr(stmt->for_step, out);
    collectCalledLambdasExpr(stmt->while_cond, out);
    collectCalledLambdasExpr(stmt->switch_expr, out);
    for (const auto& child : stmt->if_then) collectCalledLambdasStmt(child, out);
    for (const auto& child : stmt->if_else) collectCalledLambdasStmt(child, out);
    for (const auto& child : stmt->for_body) collectCalledLambdasStmt(child, out);
    for (const auto& child : stmt->while_body) collectCalledLambdasStmt(child, out);
    for (const auto& child : stmt->block_stmts) collectCalledLambdasStmt(child, out);
    for (const auto& clause : stmt->switch_cases) {
        if (clause.value) collectCalledLambdasExpr(*clause.value, out);
        for (const auto& child : clause.body) collectCalledLambdasStmt(child, out);
    }
    if (stmt->return_value) collectCalledLambdasExpr(*stmt->return_value, out);
    collectCalledLambdasExpr(stmt->expr_stmt, out);
}

static void collectCalledLambdasExpr(const ExprPtr& expr,
                                     std::unordered_set<std::string>& out) {
    if (!expr) return;
    if (expr->kind == ExprKind::Call && known_lambda_names.count(expr->callee)) {
        out.insert(expr->callee);
    }
    for (const auto& child : {expr->left, expr->right, expr->operand, expr->array_base,
                              expr->index, expr->struct_base, expr->cast_expr, expr->cond,
                              expr->then_expr, expr->else_expr, expr->base, expr->value}) {
        collectCalledLambdasExpr(child, out);
    }
    for (const auto& arg : expr->args) collectCalledLambdasExpr(arg, out);
    for (const auto& part : expr->parts) collectCalledLambdasExpr(part, out);
}

static std::unordered_set<std::string> collectCalledLambdas(const FunctionAST& fn) {
    std::unordered_set<std::string> out;
    for (const auto& stmt : fn.body) collectCalledLambdasStmt(stmt, out);
    return out;
}

static void collectLambdaCallArgVarsStmt(const StmtPtr& stmt,
                                         std::unordered_set<std::string>& out) {
    if (!stmt) return;
    for (const auto& expr : {stmt->assign_target, stmt->assign_value, stmt->if_cond,
                             stmt->for_cond, stmt->for_step, stmt->while_cond,
                             stmt->switch_expr, stmt->expr_stmt}) {
        collectLambdaCallArgVarsExpr(expr, out);
    }
    if (stmt->decl_init) collectLambdaCallArgVarsExpr(*stmt->decl_init, out);
    for (const auto& arg : stmt->decl_init_args) collectLambdaCallArgVarsExpr(arg, out);
    if (stmt->for_init) collectLambdaCallArgVarsStmt(stmt->for_init, out);
    for (const auto& child : stmt->if_then) collectLambdaCallArgVarsStmt(child, out);
    for (const auto& child : stmt->if_else) collectLambdaCallArgVarsStmt(child, out);
    for (const auto& child : stmt->for_body) collectLambdaCallArgVarsStmt(child, out);
    for (const auto& child : stmt->while_body) collectLambdaCallArgVarsStmt(child, out);
    for (const auto& child : stmt->block_stmts) collectLambdaCallArgVarsStmt(child, out);
    for (const auto& clause : stmt->switch_cases) {
        if (clause.value) collectLambdaCallArgVarsExpr(*clause.value, out);
        for (const auto& child : clause.body) collectLambdaCallArgVarsStmt(child, out);
    }
    if (stmt->return_value) collectLambdaCallArgVarsExpr(*stmt->return_value, out);
}

static void collectLambdaCallArgVarsExpr(const ExprPtr& expr,
                                         std::unordered_set<std::string>& out) {
    if (!expr) return;
    if (expr->kind == ExprKind::Call && known_lambda_names.count(expr->callee)) {
        for (const auto& arg : expr->args) {
            if (arg && arg->kind == ExprKind::VarRef && !arg->var_name.empty()) {
                out.insert(arg->var_name);
            }
        }
        if (auto name = singleIdentifierCallArgFromText(expr->literal_value)) {
            out.insert(*name);
        }
        std::vector<ExprPtr> token_args;
        appendIdentifierCallArgsFromText(token_args, expr->literal_value);
        for (const auto& arg : token_args) {
            if (arg && arg->kind == ExprKind::VarRef && !arg->var_name.empty()) {
                out.insert(arg->var_name);
            }
        }
    }
    for (const auto& child : {expr->left, expr->right, expr->operand, expr->array_base,
                              expr->index, expr->struct_base, expr->cast_expr, expr->cond,
                              expr->then_expr, expr->else_expr, expr->base, expr->value}) {
        collectLambdaCallArgVarsExpr(child, out);
    }
    for (const auto& arg : expr->args) collectLambdaCallArgVarsExpr(arg, out);
    for (const auto& part : expr->parts) collectLambdaCallArgVarsExpr(part, out);
}

static std::unordered_set<std::string> collectLambdaCallArgVars(const FunctionAST& fn) {
    std::unordered_set<std::string> out;
    for (const auto& stmt : fn.body) collectLambdaCallArgVarsStmt(stmt, out);
    return out;
}

static void collectVarRefsExpr(const ExprPtr& expr, std::unordered_set<std::string>& out);

static void collectVarRefsStmt(const StmtPtr& stmt, std::unordered_set<std::string>& out) {
    if (!stmt) return;
    for (const auto& expr : {stmt->assign_target, stmt->assign_value, stmt->if_cond,
                             stmt->for_cond, stmt->for_step, stmt->while_cond,
                             stmt->switch_expr, stmt->expr_stmt}) {
        collectVarRefsExpr(expr, out);
    }
    if (stmt->decl_init) collectVarRefsExpr(*stmt->decl_init, out);
    for (const auto& arg : stmt->decl_init_args) collectVarRefsExpr(arg, out);
    if (stmt->for_init) collectVarRefsStmt(stmt->for_init, out);
    for (const auto& child : stmt->if_then) collectVarRefsStmt(child, out);
    for (const auto& child : stmt->if_else) collectVarRefsStmt(child, out);
    for (const auto& child : stmt->for_body) collectVarRefsStmt(child, out);
    for (const auto& child : stmt->while_body) collectVarRefsStmt(child, out);
    for (const auto& child : stmt->block_stmts) collectVarRefsStmt(child, out);
    for (const auto& clause : stmt->switch_cases) {
        if (clause.value) collectVarRefsExpr(*clause.value, out);
        for (const auto& child : clause.body) collectVarRefsStmt(child, out);
    }
    if (stmt->return_value) collectVarRefsExpr(*stmt->return_value, out);
}

static void collectVarRefsExpr(const ExprPtr& expr, std::unordered_set<std::string>& out) {
    if (!expr) return;
    if (expr->kind == ExprKind::VarRef && !expr->var_name.empty()) {
        out.insert(expr->var_name);
    }
    for (const auto& child : {expr->left, expr->right, expr->operand, expr->array_base,
                              expr->index, expr->struct_base, expr->cast_expr, expr->cond,
                              expr->then_expr, expr->else_expr, expr->base, expr->value}) {
        collectVarRefsExpr(child, out);
    }
    for (const auto& arg : expr->args) collectVarRefsExpr(arg, out);
    for (const auto& part : expr->parts) collectVarRefsExpr(part, out);
}

static std::unordered_set<std::string> collectVarRefs(const FunctionAST& fn) {
    std::unordered_set<std::string> out;
    for (const auto& stmt : fn.body) collectVarRefsStmt(stmt, out);
    return out;
}

static void collectLocalDeclNamesStmt(const StmtPtr& stmt, std::unordered_set<std::string>& out) {
    if (!stmt) return;
    if (stmt->kind == StmtKind::Decl && !stmt->decl_name.empty()) out.insert(stmt->decl_name);
    if (stmt->for_init) collectLocalDeclNamesStmt(stmt->for_init, out);
    for (const auto& child : stmt->if_then) collectLocalDeclNamesStmt(child, out);
    for (const auto& child : stmt->if_else) collectLocalDeclNamesStmt(child, out);
    for (const auto& child : stmt->for_body) collectLocalDeclNamesStmt(child, out);
    for (const auto& child : stmt->while_body) collectLocalDeclNamesStmt(child, out);
    for (const auto& child : stmt->block_stmts) collectLocalDeclNamesStmt(child, out);
    for (const auto& clause : stmt->switch_cases) {
        for (const auto& child : clause.body) collectLocalDeclNamesStmt(child, out);
    }
}

static std::unordered_set<std::string> collectLocalDeclNames(const FunctionAST& fn) {
    std::unordered_set<std::string> out;
    for (const auto& stmt : fn.body) collectLocalDeclNamesStmt(stmt, out);
    return out;
}

static std::unordered_map<std::string, ParamDecl> collectVisibleParams(const FunctionAST& root) {
    std::unordered_map<std::string, ParamDecl> out;
    auto collect_stmts = [&](const std::vector<StmtPtr>& stmts, auto& self) -> void {
        for (const auto& stmt : stmts) {
            if (!stmt) continue;
            if (stmt->kind == StmtKind::Decl && !stmt->decl_name.empty()) {
                ParamDecl param;
                param.name = stmt->decl_name;
                param.type = stmt->decl_type;
                param.passing = ParamPassingKind::MutableRef;
                param.direction = ParamDirection::Output;
                param.is_output = true;
                param.is_reference = true;
                out.insert_or_assign(param.name, std::move(param));
            }
            if (stmt->for_init) self(std::vector<StmtPtr>{stmt->for_init}, self);
            self(stmt->if_then, self);
            self(stmt->if_else, self);
            self(stmt->for_body, self);
            self(stmt->while_body, self);
            self(stmt->block_stmts, self);
            for (const auto& clause : stmt->switch_cases) self(clause.body, self);
        }
    };
    auto collect = [&](const FunctionAST& fn, auto& self) -> void {
        for (const auto& param : fn.params) {
            if (!param.name.empty()) out.insert_or_assign(param.name, param);
        }
        collect_stmts(fn.body, collect_stmts);
        for (const auto& helper : fn.helpers) {
            if (helper) self(*helper, self);
        }
        for (const auto& [_, lambda] : fn.lambdas) {
            if (lambda) self(*lambda, self);
        }
    };
    collect(root, collect);
    return out;
}

static void propagateLambdaCaptureDependencies(FunctionAST& root) {
    std::unordered_map<std::string, FunctionAST*> lambdas;
    collectFunctionLambdas(root, lambdas);
    auto visible_params = collectVisibleParams(root);
    const int max_iterations = static_cast<int>(lambdas.size() * lambdas.size() + 1);
    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        bool changed = false;
        for (auto& [caller_name, caller] : lambdas) {
            if (!caller) continue;
            auto local_decls = collectLocalDeclNames(*caller);
            auto called = collectCalledLambdas(*caller);
            for (const auto& callee_name : called) {
                auto capture_it = lambda_captures_by_name.find(callee_name);
                if (capture_it == lambda_captures_by_name.end()) continue;
                for (const auto& capture : capture_it->second) {
                    if (local_decls.count(capture.name)) continue;
                    if (addTransitiveCaptureParam(*caller, caller_name, capture)) {
                        changed = true;
                    }
                }
            }
            auto external_refs = collectLambdaCallArgVars(*caller);
            auto all_refs = collectVarRefs(*caller);
            external_refs.insert(all_refs.begin(), all_refs.end());
            for (const auto& var_name : external_refs) {
                if (hasParamNamed(*caller, var_name)) continue;
                if (local_decls.count(var_name)) continue;
                auto param_it = visible_params.find(var_name);
                if (param_it == visible_params.end()) continue;
                LambdaCaptureInfo capture;
                capture.name = var_name;
                capture.type = param_it->second.type;
                capture.passing = param_it->second.passing;
                capture.direction = param_it->second.direction;
                capture.is_reference = param_it->second.is_reference;
                capture.is_const = param_it->second.is_const;
                if (addTransitiveCaptureParam(*caller, caller_name, capture)) {
                    changed = true;
                }
            }
        }
        if (!changed) return;
    }
    failUnsupported(clang_getNullCursor(),
                    "S0 lambda capture dependency propagation did not converge");
}

static void normalizeLambdaCallArgsExpr(const ExprPtr& expr);

static void normalizeLambdaCallArgsStmt(const StmtPtr& stmt) {
    if (!stmt) return;
    normalizeLambdaCallArgsExpr(stmt->assign_target);
    normalizeLambdaCallArgsExpr(stmt->assign_value);
    if (stmt->decl_init) normalizeLambdaCallArgsExpr(*stmt->decl_init);
    for (auto& arg : stmt->decl_init_args) normalizeLambdaCallArgsExpr(arg);
    normalizeLambdaCallArgsExpr(stmt->if_cond);
    if (stmt->for_init) normalizeLambdaCallArgsStmt(stmt->for_init);
    normalizeLambdaCallArgsExpr(stmt->for_cond);
    normalizeLambdaCallArgsExpr(stmt->for_step);
    normalizeLambdaCallArgsExpr(stmt->while_cond);
    normalizeLambdaCallArgsExpr(stmt->switch_expr);
    for (auto& child : stmt->if_then) normalizeLambdaCallArgsStmt(child);
    for (auto& child : stmt->if_else) normalizeLambdaCallArgsStmt(child);
    for (auto& child : stmt->for_body) normalizeLambdaCallArgsStmt(child);
    for (auto& child : stmt->while_body) normalizeLambdaCallArgsStmt(child);
    for (auto& child : stmt->block_stmts) normalizeLambdaCallArgsStmt(child);
    for (auto& clause : stmt->switch_cases) {
        if (clause.value) normalizeLambdaCallArgsExpr(*clause.value);
        for (auto& child : clause.body) normalizeLambdaCallArgsStmt(child);
    }
    if (stmt->return_value) normalizeLambdaCallArgsExpr(*stmt->return_value);
    normalizeLambdaCallArgsExpr(stmt->expr_stmt);
}

static void normalizeOneLambdaCallArgs(Expr& expr) {
    auto capture_it = lambda_captures_by_name.find(expr.callee);
    if (capture_it == lambda_captures_by_name.end()) return;
    const auto& captures = capture_it->second;
    if (expr.args.empty() && !expr.literal_value.empty()) {
        appendSingleIdentifierCallArgFromText(expr.args, expr.literal_value);
        if (expr.args.empty()) appendIdentifierCallArgsFromText(expr.args, expr.literal_value);
    }
    if (!expr.literal_value.empty() && expr.args.size() <= captures.size()) {
        std::vector<ExprPtr> parsed_args;
        appendIdentifierCallArgsFromText(parsed_args, expr.literal_value);
        if (!parsed_args.empty()) {
            auto param_count_it = lambda_param_count_by_name.find(expr.callee);
            std::size_t param_count = param_count_it == lambda_param_count_by_name.end()
                ? std::numeric_limits<std::size_t>::max()
                : param_count_it->second;
            if (expr.args.size() >= param_count) parsed_args.clear();
            if (expr.args.size() + parsed_args.size() > param_count) {
                parsed_args.resize(param_count - expr.args.size());
            }
            bool already_has_tail = parsed_args.size() <= expr.args.size();
            if (already_has_tail) {
                std::size_t offset = expr.args.size() - parsed_args.size();
                for (std::size_t i = 0; i < parsed_args.size(); ++i) {
                    const auto& existing = expr.args[offset + i];
                    const auto& parsed = parsed_args[i];
                    if (!existing || !parsed ||
                        existing->kind != ExprKind::VarRef ||
                        parsed->kind != ExprKind::VarRef ||
                        existing->var_name != parsed->var_name) {
                        already_has_tail = false;
                        break;
                    }
                }
            }
            if (!already_has_tail) {
                expr.args.insert(expr.args.end(), parsed_args.begin(), parsed_args.end());
            }
        }
    }
    std::size_t existing_capture_prefix = 0;
    while (existing_capture_prefix < captures.size() &&
           existing_capture_prefix < expr.args.size()) {
        const auto& arg = expr.args[existing_capture_prefix];
        if (!arg || arg->kind != ExprKind::VarRef ||
            arg->var_name != captures[existing_capture_prefix].name) {
            break;
        }
        ++existing_capture_prefix;
    }

    std::vector<ExprPtr> explicit_args;
    explicit_args.reserve(expr.args.size() - existing_capture_prefix);
    for (std::size_t i = existing_capture_prefix; i < expr.args.size(); ++i) {
        explicit_args.push_back(expr.args[i]);
    }

    std::vector<ExprPtr> rebuilt;
    rebuilt.reserve(captures.size() + explicit_args.size());
    for (const auto& capture : captures) {
        rebuilt.push_back(makeCaptureArg(capture, expr.debug_loc));
    }
    rebuilt.insert(rebuilt.end(), explicit_args.begin(), explicit_args.end());
    auto param_count_it = lambda_param_count_by_name.find(expr.callee);
    if (param_count_it != lambda_param_count_by_name.end() &&
        rebuilt.size() > param_count_it->second) {
        rebuilt.resize(param_count_it->second);
    }
    expr.args = std::move(rebuilt);
}

static void normalizeLambdaCallArgsExpr(const ExprPtr& expr) {
    if (!expr) return;
    for (const auto& child : {expr->left, expr->right, expr->operand, expr->array_base,
                              expr->index, expr->struct_base, expr->cast_expr, expr->cond,
                              expr->then_expr, expr->else_expr, expr->base, expr->value}) {
        normalizeLambdaCallArgsExpr(child);
    }
    for (auto& arg : expr->args) normalizeLambdaCallArgsExpr(arg);
    for (auto& part : expr->parts) normalizeLambdaCallArgsExpr(part);
    if (expr->kind == ExprKind::Call && known_lambda_names.count(expr->callee)) {
        normalizeOneLambdaCallArgs(*expr);
    }
}

static void normalizeLambdaCallsInFunction(FunctionAST& fn) {
    for (auto& stmt : fn.body) normalizeLambdaCallArgsStmt(stmt);
    for (auto& [_, lambda] : fn.lambdas) {
        if (lambda) normalizeLambdaCallsInFunction(*lambda);
    }
    for (auto& helper : fn.helpers) {
        if (helper) normalizeLambdaCallsInFunction(*helper);
    }
}

static std::string implicitPortParamName(const std::string& port_name) {
    return "__rtlzz_port_" + port_name;
}

template <typename Fn>
static void walkPortExpr(const ExprPtr& expr, Fn& fn) {
    if (!expr) return;
    fn(expr);
    for (const auto& child : {expr->left, expr->right, expr->operand, expr->array_base,
                              expr->index, expr->struct_base, expr->cast_expr, expr->cond,
                              expr->then_expr, expr->else_expr, expr->base, expr->value}) {
        walkPortExpr(child, fn);
    }
    for (const auto& arg : expr->args) walkPortExpr(arg, fn);
    for (const auto& part : expr->parts) walkPortExpr(part, fn);
}

template <typename Fn>
static void walkPortStmt(const StmtPtr& stmt, Fn& fn) {
    if (!stmt) return;
    for (const auto& expr : {stmt->assign_target, stmt->assign_value, stmt->if_cond,
                             stmt->for_cond, stmt->for_step, stmt->while_cond,
                             stmt->switch_expr, stmt->expr_stmt}) {
        walkPortExpr(expr, fn);
    }
    if (stmt->decl_init) walkPortExpr(*stmt->decl_init, fn);
    for (const auto& arg : stmt->decl_init_args) walkPortExpr(arg, fn);
    if (stmt->for_init) walkPortStmt(stmt->for_init, fn);
    for (const auto& child : stmt->if_then) walkPortStmt(child, fn);
    for (const auto& child : stmt->if_else) walkPortStmt(child, fn);
    for (const auto& child : stmt->for_body) walkPortStmt(child, fn);
    for (const auto& child : stmt->while_body) walkPortStmt(child, fn);
    for (const auto& child : stmt->block_stmts) walkPortStmt(child, fn);
    for (const auto& clause : stmt->switch_cases) {
        if (clause.value) walkPortExpr(*clause.value, fn);
        for (const auto& child : clause.body) walkPortStmt(child, fn);
    }
    if (stmt->return_value) walkPortExpr(*stmt->return_value, fn);
}

template <typename Fn>
static void walkPortFunctionBody(FunctionAST& function, Fn&& fn) {
    for (const auto& stmt : function.body) walkPortStmt(stmt, fn);
}

static bool portLiftTypeMatches(const TypeInfo& lhs, const TypeInfo& rhs) {
    return (lhs.width == 0 || rhs.width == 0 || lhs.width == rhs.width) &&
           lhs.is_array == rhs.is_array &&
           lhs.array_dims == rhs.array_dims;
}

static void repairRecoveredExprTypes(const ExprPtr& expr,
                                     const TypeInfo& expected) {
    if (!expr) return;
    const bool aggregate_mismatch =
        (expr->kind == ExprKind::BinaryOp || expr->kind == ExprKind::UnaryOp) &&
        (expr->type.is_array || !expr->type.struct_name.empty()) &&
        !expected.is_array && expected.struct_name.empty();
    if (aggregate_mismatch ||
        expr->type.name.empty() || expr->type.name == "unknown" ||
        (expr->type.width == 0 && !expr->type.is_array &&
         expr->type.struct_name.empty())) {
        expr->type = expected;
    }
    if (expr->kind == ExprKind::BinaryOp) {
        repairRecoveredExprTypes(expr->left, expected);
        repairRecoveredExprTypes(expr->right, expected);
    } else if (expr->kind == ExprKind::UnaryOp) {
        repairRecoveredExprTypes(expr->operand, expected);
    } else if (expr->kind == ExprKind::ArrayAccess) {
        // The recovered base normally receives its aggregate type from the
        // function-local symbol table; the selected element has the expected
        // call-parameter type.
        if (expr->array_base &&
            (expr->array_base->type.name.empty() ||
             expr->array_base->type.name == "unknown")) {
            expr->array_base->type = expected;
        }
    }
}

static void collectAllSurfaceFunctions(
    FunctionAST& top,
    std::vector<FunctionAST*>& functions) {
    functions.push_back(&top);
    auto collect = [&](FunctionAST& fn, auto& self) -> void {
        for (auto& [_, lambda] : fn.lambdas) {
            if (!lambda) continue;
            functions.push_back(lambda.get());
            self(*lambda, self);
        }
        for (auto& helper : fn.helpers) {
            if (!helper) continue;
            functions.push_back(helper.get());
            self(*helper, self);
        }
    };
    collect(top, collect);
}

static void liftGlobalPortsIntoFunctions(FunctionAST& top) {
    std::vector<FunctionAST*> functions;
    collectAllSurfaceFunctions(top, functions);

    std::unordered_map<std::string, std::vector<FunctionAST*>> functions_by_name;
    for (auto* function : functions) {
        functions_by_name[function->name].push_back(function);
    }

    using PortSet = std::unordered_set<std::string>;
    std::unordered_map<FunctionAST*, PortSet> required_ports;
    std::unordered_map<Expr*, FunctionAST*> resolved_calls;

    for (auto* function : functions) {
        auto collect_direct = [&](const ExprPtr& expr) {
            if (!expr->global_port_name.empty()) {
                required_ports[function].insert(expr->global_port_name);
            }
        };
        walkPortFunctionBody(*function, collect_direct);
    }

    // Resolve calls while every function still has its source-visible signature.
    for (auto* caller : functions) {
        auto resolve_call = [&](const ExprPtr& expr) {
            if (expr->kind != ExprKind::Call) return;
            auto named = functions_by_name.find(expr->callee);
            if (named == functions_by_name.end()) return;
            if (named->second.size() == 1 &&
                named->second.front()->params.size() == expr->args.size()) {
                resolved_calls[expr.get()] = named->second.front();
                expr->type = named->second.front()->return_type;
                for (std::size_t i = 0; i < expr->args.size(); ++i) {
                    repairRecoveredExprTypes(
                        expr->args[i], named->second.front()->params[i].type);
                }
                return;
            }
            FunctionAST* match = nullptr;
            for (auto* candidate : named->second) {
                if (candidate->params.size() != expr->args.size()) continue;
                bool types_match = true;
                for (std::size_t i = 0; i < expr->args.size(); ++i) {
                    if (!expr->args[i] ||
                        !portLiftTypeMatches(candidate->params[i].type,
                                             expr->args[i]->type)) {
                        types_match = false;
                        break;
                    }
                }
                if (!types_match) continue;
                if (match) {
                    // Leave ambiguity to S2's existing diagnostic.
                    return;
                }
                match = candidate;
            }
            if (match) {
                resolved_calls[expr.get()] = match;
                expr->type = match->return_type;
                for (std::size_t i = 0; i < expr->args.size(); ++i) {
                    repairRecoveredExprTypes(expr->args[i], match->params[i].type);
                }
            }
        };
        walkPortFunctionBody(*caller, resolve_call);
    }

    // Some libclang recovery cursors cover both a lambda call and its
    // trailing binary expression and report the receiver aggregate type.
    // Once callees are resolved, repair such impossible aggregate arithmetic
    // from the now-known scalar operand/return types.
    for (int iteration = 0; iteration < 4; ++iteration) {
        for (auto* function : functions) {
            auto repair_ops = [&](const ExprPtr& expr) {
                if (expr->kind != ExprKind::BinaryOp &&
                    expr->kind != ExprKind::UnaryOp) {
                    return;
                }
                if (!expr->type.is_array && expr->type.struct_name.empty()) return;
                const ExprPtr candidate = expr->kind == ExprKind::BinaryOp
                    ? (expr->left && !expr->left->type.is_array &&
                       expr->left->type.struct_name.empty()
                           ? expr->left
                           : expr->right)
                    : expr->operand;
                if (candidate && !candidate->type.is_array &&
                    candidate->type.struct_name.empty() &&
                    candidate->type.width > 0) {
                    expr->type = candidate->type;
                }
            };
            walkPortFunctionBody(*function, repair_ops);
        }
    }

    const std::size_t max_iterations = functions.size() * functions.size() + 1;
    for (std::size_t iteration = 0; iteration < max_iterations; ++iteration) {
        bool changed = false;
        for (auto* caller : functions) {
            auto propagate = [&](const ExprPtr& expr) {
                auto callee = resolved_calls.find(expr.get());
                if (callee == resolved_calls.end()) return;
                for (const auto& port : required_ports[callee->second]) {
                    changed |= required_ports[caller].insert(port).second;
                }
            };
            walkPortFunctionBody(*caller, propagate);
        }
        if (!changed) break;
        if (iteration + 1 == max_iterations) {
            failUnsupported(clang_getNullCursor(),
                            "S0 global port dependency propagation did not converge");
        }
    }

    for (auto* function : functions) {
        const bool is_top = function == &top;
        if (!is_top) {
            for (const auto& port : global_ports_in_source_order) {
                if (!required_ports[function].count(port.name)) continue;
                ParamDecl implicit = port;
                implicit.name = implicitPortParamName(port.name);
                function->params.push_back(std::move(implicit));
            }
        }
        auto rewrite_refs = [&](const ExprPtr& expr) {
            if (expr->global_port_name.empty()) return;
            if (!is_top) expr->var_name = implicitPortParamName(expr->global_port_name);
            expr->global_port_name.clear();
        };
        walkPortFunctionBody(*function, rewrite_refs);
    }

    for (auto* caller : functions) {
        const bool caller_is_top = caller == &top;
        auto append_port_args = [&](const ExprPtr& expr) {
            auto callee = resolved_calls.find(expr.get());
            if (callee == resolved_calls.end()) return;
            for (const auto& port : global_ports_in_source_order) {
                if (!required_ports[callee->second].count(port.name)) continue;
                auto argument = make_var(
                    caller_is_top ? port.name : implicitPortParamName(port.name),
                    port.type);
                argument->debug_loc = expr->debug_loc;
                expr->args.push_back(std::move(argument));
            }
        };
        walkPortFunctionBody(*caller, append_port_args);
    }
}

static StmtPtr convertStaticArrayVarDecl(CXCursor c) {
    if (clang_getCursorKind(c) != CXCursor_VarDecl) return nullptr;
    TypeInfo type = convertType(clang_getCursorType(c));
    if (!type.is_array) return nullptr;
    if (clang_Cursor_getStorageClass(c) != CX_SC_Static) return nullptr;
    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Decl;
    stmt->decl_name = cxToStr(clang_getCursorSpelling(c));
    stmt->decl_type = type;
    stmt->decl_type.is_static = true;
    for (const auto& child : getChildren(c)) {
        if (clang_isExpression(clang_getCursorKind(child))) {
            collectIntegerInitValues(child, stmt->decl_type.init_values);
            break;
        }
    }
    return stmt;
}

static StmtPtr convertGlobalConstScalarDecl(CXCursor c) {
    if (clang_getCursorKind(c) != CXCursor_VarDecl) return nullptr;
    CXType cursor_type = clang_getCursorType(c);
    TypeInfo type = convertType(cursor_type);
    if (type.is_array || !clang_isConstQualifiedType(cursor_type)) return nullptr;
    CXEvalResult eval = clang_Cursor_Evaluate(c);
    if (!eval) return nullptr;
    CXEvalResultKind kind = clang_EvalResult_getKind(eval);
    if (kind != CXEval_Int) {
        clang_EvalResult_dispose(eval);
        return nullptr;
    }
    std::string value = type.is_signed
        ? std::to_string(clang_EvalResult_getAsLongLong(eval))
        : std::to_string(clang_EvalResult_getAsUnsigned(eval));
    clang_EvalResult_dispose(eval);

    auto stmt = std::make_shared<Stmt>();
    stmt->kind = StmtKind::Decl;
    stmt->decl_name = cxToStr(clang_getCursorSpelling(c));
    stmt->decl_type = type;
    stmt->decl_init = make_literal(value, type);
    return stmt;
}

static std::string firstDeclRefName(CXCursor cursor) {
    if (clang_getCursorKind(cursor) == CXCursor_DeclRefExpr) {
        return cxToStr(clang_getCursorSpelling(cursor));
    }
    std::string found;
    clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto* out = static_cast<std::string*>(data);
        if (!out->empty()) return CXChildVisit_Break;
        if (clang_getCursorKind(child) == CXCursor_DeclRefExpr) {
            *out = cxToStr(clang_getCursorSpelling(child));
            return CXChildVisit_Break;
        }
        std::string nested = firstDeclRefName(child);
        if (!nested.empty()) {
            *out = std::move(nested);
            return CXChildVisit_Break;
        }
        return CXChildVisit_Continue;
    }, &found);
    return found;
}

static StructConstructorInfo collectStructConstructorInfo(CXCursor ctor) {
    StructConstructorInfo info;
    auto children = getChildren(ctor);
    std::unordered_set<std::string> params;
    for (auto child : children) {
        if (clang_getCursorKind(child) == CXCursor_ParmDecl) {
            std::string name = cxToStr(clang_getCursorSpelling(child));
            info.param_names.push_back(name);
            if (!name.empty()) params.insert(std::move(name));
        }
    }
    for (size_t i = 0; i < children.size(); ++i) {
        auto kind = clang_getCursorKind(children[i]);
        if (kind != CXCursor_MemberRef && kind != CXCursor_MemberRefExpr) continue;
        std::string field = cxToStr(clang_getCursorSpelling(children[i]));
        if (field.empty()) continue;
        std::string param = firstDeclRefName(children[i]);
        if (param.empty() || !params.count(param)) {
            for (size_t j = i + 1; j < children.size(); ++j) {
                auto next_kind = clang_getCursorKind(children[j]);
                if (next_kind == CXCursor_MemberRef || next_kind == CXCursor_MemberRefExpr) break;
                param = firstDeclRefName(children[j]);
                if (!param.empty() && params.count(param)) break;
            }
        }
        if (!param.empty() && params.count(param)) info.field_to_param[field] = param;
    }
    return info;
}

static void registerStructMetadata(FunctionAST& func,
                                   const std::string& name,
                                   const std::vector<StructFieldInfo>& fields,
                                   const std::vector<StructConstructorInfo>& constructors,
                                   CXCursor cursor) {
    if (name.empty()) return;
    func.struct_fields[name] = fields;
    func.struct_fields["struct " + name] = fields;
    if (!constructors.empty()) {
        func.struct_constructors[name] = constructors;
        func.struct_constructors["struct " + name] = constructors;
    }
    CXType type = clang_getCursorType(cursor);
    std::string spelling = cxToStr(clang_getTypeSpelling(type));
    if (!spelling.empty()) {
        func.struct_fields[spelling] = fields;
        if (!constructors.empty()) func.struct_constructors[spelling] = constructors;
    }
}

static void collectStructFieldLayouts(CXCursor root, FunctionAST& func) {
    clang_visitChildren(root, [](CXCursor c, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto* func = static_cast<FunctionAST*>(data);
        auto kind = clang_getCursorKind(c);
        if ((kind == CXCursor_StructDecl || kind == CXCursor_ClassDecl) &&
            clang_isCursorDefinition(c)) {
            std::string name = cxToStr(clang_getCursorSpelling(c));
            if (!name.empty()) {
                std::vector<StructFieldInfo> fields;
                std::vector<StructConstructorInfo> constructors;
                struct StructCollectState {
                    std::vector<StructFieldInfo>* fields;
                    std::vector<StructConstructorInfo>* constructors;
                } state{&fields, &constructors};
                clang_visitChildren(c, [](CXCursor child, CXCursor, CXClientData field_data) -> CXChildVisitResult {
                    auto* state = static_cast<StructCollectState*>(field_data);
                    auto child_kind = clang_getCursorKind(child);
                    if (child_kind == CXCursor_FieldDecl) {
                        StructFieldInfo info;
                        info.name = cxToStr(clang_getCursorSpelling(child));
                        info.type = convertType(clang_getCursorType(child));
                        state->fields->push_back(info);
                        return CXChildVisit_Continue;
                    }
                    if (child_kind == CXCursor_Constructor) {
                        auto ctor = collectStructConstructorInfo(child);
                        if (!ctor.param_names.empty() && !ctor.field_to_param.empty()) {
                            state->constructors->push_back(std::move(ctor));
                        }
                    }
                    return CXChildVisit_Continue;
                }, &state);
                if (!fields.empty()) {
                    registerStructMetadata(*func, name, fields, constructors, c);
                }
            }
        }
        return CXChildVisit_Recurse;
    }, &func);
}

static void collectGlobalConstInts(CXCursor root, const std::string& source_file) {
    std::string wanted = normalizedPath(source_file);
    clang_visitChildren(root, [](CXCursor c, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto* wanted = static_cast<std::string*>(data);
        if (clang_getCursorKind(c) == CXCursor_EnumConstantDecl) {
            if (normalizedPath(cursorFileName(c)) != *wanted) return CXChildVisit_Continue;
            std::string name = cxToStr(clang_getCursorSpelling(c));
            if (!name.empty()) {
                global_const_int_values[name] = clang_getEnumConstantDeclValue(c);
            }
            return CXChildVisit_Continue;
        }
        if (clang_getCursorKind(c) != CXCursor_VarDecl) return CXChildVisit_Recurse;
        if (normalizedPath(cursorFileName(c)) != *wanted) return CXChildVisit_Continue;
        CXType cursor_type = clang_getCursorType(c);
        if (!clang_isConstQualifiedType(cursor_type)) return CXChildVisit_Continue;
        TypeInfo type = convertType(cursor_type);
        if (type.is_array) return CXChildVisit_Continue;
        CXEvalResult eval = clang_Cursor_Evaluate(c);
        if (!eval) return CXChildVisit_Continue;
        CXEvalResultKind kind = clang_EvalResult_getKind(eval);
        if (kind == CXEval_Int) {
            std::string name = cxToStr(clang_getCursorSpelling(c));
            if (!name.empty()) {
                global_const_int_values[name] = type.is_signed
                    ? clang_EvalResult_getAsLongLong(eval)
                    : static_cast<long long>(clang_EvalResult_getAsUnsigned(eval));
            }
        }
        clang_EvalResult_dispose(eval);
        return CXChildVisit_Continue;
    }, &wanted);
}

static std::vector<std::string> constIntExprTokens(const std::string& expr) {
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < expr.size();) {
        unsigned char ch = static_cast<unsigned char>(expr[i]);
        if (std::isspace(ch)) {
            ++i;
            continue;
        }
        if (expr[i] == '+' || expr[i] == '-') {
            tokens.push_back(std::string(1, expr[i++]));
            continue;
        }
        if (std::isalnum(ch) || expr[i] == '_') {
            std::size_t begin = i++;
            while (i < expr.size()) {
                unsigned char next = static_cast<unsigned char>(expr[i]);
                if (!std::isalnum(next) && expr[i] != '_') break;
                ++i;
            }
            tokens.push_back(expr.substr(begin, i - begin));
            continue;
        }
        ++i;
    }
    return tokens;
}

static void collectScopedConstIntsFromText(const std::string& text) {
    static const std::regex decl_pattern(
        R"(\b(?:constexpr|const)\s+[^;=(){}]*?\b([A-Za-z_][A-Za-z_0-9]*)\s*=\s*([^;]+);)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), decl_pattern);
         it != std::sregex_iterator(); ++it) {
        std::string name = (*it)[1].str();
        std::string expr = (*it)[2].str();
        auto value = evalTemplateIntTokens(constIntExprTokens(expr));
        if (value) global_const_int_values[name] = *value;
    }
}

static void collectScopedConstInts(CXCursor root, const std::string& source_file) {
    collectScopedConstIntsFromText(cursorText(root));
    std::string wanted = normalizedPath(source_file);
    clang_visitChildren(root, [](CXCursor c, CXCursor,
                                 CXClientData data) -> CXChildVisitResult {
        auto* wanted = static_cast<std::string*>(data);
        if (clang_getCursorKind(c) != CXCursor_VarDecl) return CXChildVisit_Recurse;
        if (normalizedPath(cursorFileName(c)) != *wanted) return CXChildVisit_Continue;
        CXType cursor_type = clang_getCursorType(c);
        TypeInfo type = convertType(cursor_type);
        if (type.is_array) return CXChildVisit_Continue;
        CXEvalResult eval = clang_Cursor_Evaluate(c);
        if (!eval) return CXChildVisit_Continue;
        CXEvalResultKind kind = clang_EvalResult_getKind(eval);
        if (kind == CXEval_Int) {
            std::string name = cxToStr(clang_getCursorSpelling(c));
            if (!name.empty()) {
                global_const_int_values[name] = type.is_signed
                    ? clang_EvalResult_getAsLongLong(eval)
                    : static_cast<long long>(clang_EvalResult_getAsUnsigned(eval));
            }
        }
        clang_EvalResult_dispose(eval);
        return CXChildVisit_Continue;
    }, &wanted);
}

struct SourcePortPragma {
    ParamDirection direction = ParamDirection::Input;
    int line = 0;
};

static std::unordered_map<std::string, SourcePortPragma>
parseSourcePortPragmas(const std::string& source, std::string& error) {
    std::unordered_map<std::string, SourcePortPragma> out;
    std::regex pragma(
        R"(^\s*#\s*pragma\s+(input_port|output_port)\s+([A-Za-z_][A-Za-z0-9_]*)\s*$)");
    std::istringstream lines(source);
    std::string line;
    int line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        std::smatch match;
        if (!std::regex_match(line, match, pragma)) continue;
        std::string name = match[2].str();
        SourcePortPragma spec;
        spec.direction = match[1].str() == "input_port"
            ? ParamDirection::Input
            : ParamDirection::Output;
        spec.line = line_number;
        if (!out.emplace(name, spec).second) {
            error = "Duplicate or conflicting port pragma for global variable '" +
                    name + "'";
            return {};
        }
    }
    return out;
}

static bool supportedGlobalPortType(const TypeInfo& type) {
    if (type.is_pointer || type.is_reference || type.is_const ||
        !type.struct_name.empty()) {
        return false;
    }
    if (type.is_array) {
        TypeInfo scalar = type;
        scalar.is_array = false;
        scalar.array_size = 0;
        scalar.array_dims.clear();
        return supportedGlobalPortType(scalar);
    }
    return type.name == "bool" || type.hw_kind == "bool" ||
           type.hw_kind == "Int" || isBuiltinIntegerType(type);
}

static std::vector<ParamDecl> collectGlobalPortParams(
    CXCursor root,
    const std::string& source_file,
    const std::string& source,
    std::string& error) {
    auto pragmas = parseSourcePortPragmas(source, error);
    if (!error.empty()) return {};

    struct CollectState {
        std::string source_file;
        const std::unordered_map<std::string, SourcePortPragma>* pragmas;
        std::unordered_set<std::string> seen;
        std::vector<ParamDecl> params;
        std::string error;
    } state{normalizedPath(source_file), &pragmas, {}, {}, {}};

    clang_visitChildren(root, [](CXCursor cursor, CXCursor,
                                 CXClientData data) -> CXChildVisitResult {
        auto* state = static_cast<CollectState*>(data);
        if (!state->error.empty() ||
            clang_getCursorKind(cursor) != CXCursor_VarDecl ||
            normalizedPath(cursorFileName(cursor)) != state->source_file) {
            return CXChildVisit_Continue;
        }
        std::string name = cxToStr(clang_getCursorSpelling(cursor));
        auto pragma = state->pragmas->find(name);
        if (pragma == state->pragmas->end()) {
            state->error = "File-scope global variable '" + name +
                           "' is not declared by #pragma input_port or "
                           "#pragma output_port";
            return CXChildVisit_Break;
        }
        TypeInfo type = convertType(clang_getCursorType(cursor));
        if (!supportedGlobalPortType(type)) {
            state->error = "Unsupported global port type for '" + name +
                           "': only bool, Int<N>, builtin integers, and "
                           "std::array forms of those types are allowed";
            return CXChildVisit_Break;
        }
        std::string declaration_text = cursorText(cursor);
        std::size_t name_pos = declaration_text.rfind(name);
        std::string declaration_tail = name_pos == std::string::npos
            ? declaration_text
            : declaration_text.substr(name_pos + name.size());
        if (declaration_tail.find('=') != std::string::npos ||
            declaration_tail.find('{') != std::string::npos ||
            declaration_tail.find('(') != std::string::npos) {
            state->error = "Global port '" + name +
                           "' must not have an initializer";
            return CXChildVisit_Break;
        }
        ParamDecl param;
        param.name = name;
        param.type = type;
        param.debug_loc = debugLocFromCursor(cursor);
        param.direction = pragma->second.direction;
        param.is_output = param.direction == ParamDirection::Output;
        param.passing = param.is_output
            ? ParamPassingKind::MutableRef
            : ParamPassingKind::Value;
        param.is_reference = param.is_output;
        state->params.push_back(std::move(param));
        global_ports_by_usr[cursorUsr(cursor)] = state->params.back();
        state->seen.insert(name);
        return CXChildVisit_Continue;
    }, &state);

    if (!state.error.empty()) {
        error = std::move(state.error);
        return {};
    }
    for (const auto& [name, _] : pragmas) {
        if (!state.seen.count(name)) {
            error = "Port pragma names no file-scope global variable: '" +
                    name + "'";
            return {};
        }
    }
    global_ports_in_source_order = state.params;
    return state.params;
}

static NativeBuildResult buildV2ASTFromSourceImpl(const std::string& source_file,
                                          const std::string* source_text,
                                          const std::string& top_function,
                                          const std::vector<std::string>& extra_args) {
    NativeBuildResult result;
    lambda_operator_usr_to_name.clear();
    lambda_operator_location_to_name.clear();
    lambda_operator_signature_to_name.clear();
    lambda_var_usr_to_name.clear();
    lambda_var_location_to_name.clear();
    lambda_source_name_to_unique_name.clear();
    lambda_captures_by_name.clear();
    lambda_param_count_by_name.clear();
    known_lambda_names.clear();
    lambda_template_specialization_args_by_name.clear();
    lambda_template_specialization_name_by_key.clear();
    lambda_template_specialization_method_by_key.clear();
    lambda_unique_counter = 0;
    global_const_int_values.clear();
    current_template_int_values.clear();
    global_ports_by_usr.clear();
    global_ports_in_source_order.clear();
    helper_function_usr_to_name.clear();
    helper_template_call_to_name.clear();
    function_return_types_by_name.clear();
    source_function_templates_by_name.clear();
    function_template_specialization_name_by_key.clear();
    pending_function_template_specializations.clear();
    processed_function_template_specializations.clear();
    recovered_symbol_types.clear();

    CXIndex index = clang_createIndex(0, 0);

    std::vector<const char*> args;
    args.push_back("-std=c++17");
    args.push_back("-fsyntax-only");
    for (auto& a : extra_args) args.push_back(a.c_str());

    std::string parse_file = source_file;
    std::string source_buffer;
    std::string source_for_ports;
    std::string wrapper_source;
    std::vector<CXUnsavedFile> unsaved_storage;
    unsigned unsaved_count = 0;
    if (source_text) {
        source_for_ports = *source_text;
        source_buffer = *source_text;
        if (!sourceTextContainsFixintInclude(source_buffer)) {
            source_buffer = "#include <fixint.hpp>\n#line 1 \"" +
                            escapeForQuotedPath(source_file) + "\"\n" +
                            source_buffer;
        }
        CXUnsavedFile source_unsaved{};
        source_unsaved.Filename = source_file.c_str();
        source_unsaved.Contents = source_buffer.c_str();
        source_unsaved.Length = static_cast<unsigned long>(source_buffer.size());
        unsaved_storage.push_back(source_unsaved);
    }
    if (!source_text) {
        std::ifstream source_in(source_file);
        std::ostringstream source_stream;
        source_stream << source_in.rdbuf();
        source_for_ports = source_stream.str();
    }
    const bool has_fixint_include = source_text
        ? sourceTextContainsFixintInclude(*source_text)
        : sourceContainsFixintInclude(source_file);
    if (!source_text && !has_fixint_include) {
        parse_file = "/tmp/rtlzz_fixint_include_wrapper.cpp";
        wrapper_source = "#include <fixint.hpp>\n#include \"" +
                         includePathForSource(source_file) + "\"\n";
        CXUnsavedFile wrapper_unsaved{};
        wrapper_unsaved.Filename = parse_file.c_str();
        wrapper_unsaved.Contents = wrapper_source.c_str();
        wrapper_unsaved.Length = static_cast<unsigned long>(wrapper_source.size());
        unsaved_storage.push_back(wrapper_unsaved);
    }
    CXUnsavedFile* unsaved_files = unsaved_storage.empty() ? nullptr : unsaved_storage.data();
    unsaved_count = static_cast<unsigned>(unsaved_storage.size());

    CXTranslationUnit tu = clang_parseTranslationUnit(
        index, parse_file.c_str(),
        args.data(), static_cast<int>(args.size()),
        unsaved_files, unsaved_count,
        CXTranslationUnit_None);

    if (!tu) {
        result.error = "Failed to parse translation unit: " + source_file;
        clang_disposeIndex(index);
        return result;
    }

    // Check for errors
    unsigned numDiag = clang_getNumDiagnostics(tu);
    for (unsigned i = 0; i < numDiag; ++i) {
        CXDiagnostic diag = clang_getDiagnostic(tu, i);
        CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diag);
        if (sev >= CXDiagnostic_Error) {
            result.error = cxToStr(clang_formatDiagnostic(diag,
                clang_defaultDiagnosticDisplayOptions()));
            clang_disposeDiagnostic(diag);
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return result;
        }
        clang_disposeDiagnostic(diag);
    }

    // Find the target function
    CXCursor root = clang_getTranslationUnitCursor(tu);
    collectGlobalConstInts(root, source_file);
    struct FindCtx {
        std::string target;
        std::string source_file;
        std::vector<std::pair<std::string, CXCursor>> matches;
    } ctx;
    ctx.target = top_function;
    ctx.source_file = normalizedPath(source_file);

    clang_visitChildren(root, [](CXCursor c, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto* ctx = static_cast<FindCtx*>(data);
        if (clang_getCursorKind(c) == CXCursor_FunctionDecl) {
            std::string name = cxToStr(clang_getCursorSpelling(c));
            std::string file = normalizedPath(cursorFileName(c));
            if (clang_isCursorDefinition(c) &&
                file == ctx->source_file &&
                wildcardMatch(ctx->target, name)) {
                ctx->matches.push_back({name, c});
            }
        }
        return CXChildVisit_Continue;
    }, &ctx);

    if (ctx.matches.empty()) {
        result.error = "Function '" + top_function + "' not found in " + source_file;
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return result;
    }
    if (ctx.matches.size() > 1) {
        std::ostringstream err;
        err << "Function pattern '" << top_function << "' matched multiple functions:";
        for (const auto& match : ctx.matches) err << " " << match.first;
        result.error = err.str();
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return result;
    }
    CXCursor found = ctx.matches.front().second;
    std::string resolved_top_function = ctx.matches.front().first;

    std::string top_source_file = cursorFileName(found);
    collectSourceFunctionTemplateDefinitions(root, top_source_file);
    auto source_functions = collectSourceFunctionDefinitions(root, top_source_file);
    bool saw_top = false;
    for (const auto& fn : source_functions) {
        if (fn.name == resolved_top_function && clang_equalCursors(fn.cursor, found)) {
            saw_top = true;
            break;
        }
    }
    if (!saw_top) {
        source_functions.push_back({resolved_top_function, found});
    }
    collectLambdaOperatorNames(root);

    for (const auto& fn : source_functions) {
        std::string subset_error = checkSubset(fn.cursor, fn.name);
        if (!subset_error.empty()) {
            result.error = subset_error;
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return result;
        }
    }

    std::string recursion_error = checkHelperCallGraphRecursion(source_functions);
    if (!recursion_error.empty()) {
        result.error = recursion_error;
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return result;
    }

    FunctionAST struct_metadata;
    collectStructFieldLayouts(root, struct_metadata);
    active_struct_fields = &struct_metadata.struct_fields;
    active_struct_constructors = &struct_metadata.struct_constructors;

    std::string port_error;
    auto global_port_params = collectGlobalPortParams(
        root, top_source_file, source_for_ports, port_error);
    if (!port_error.empty()) {
        result.error = std::move(port_error);
        active_struct_fields = nullptr;
        active_struct_constructors = nullptr;
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return result;
    }

    FunctionAST func = convertFunctionDecl(found, resolved_top_function);
    func.struct_fields = struct_metadata.struct_fields;
    func.struct_constructors = struct_metadata.struct_constructors;
    if (!func.params.empty()) {
        result.error = "Top function must not declare parameters; RTL ports are "
                       "file-scope globals annotated with #pragma input_port or "
                       "#pragma output_port";
        active_struct_fields = nullptr;
        active_struct_constructors = nullptr;
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return result;
    }
    if (func.return_type.name != "void") {
        result.error = "Top function must return void";
        active_struct_fields = nullptr;
        active_struct_constructors = nullptr;
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return result;
    }
    func.params = std::move(global_port_params);
    {
        auto saved_const_int_values = global_const_int_values;
        collectScopedConstInts(found, cursorFileName(found));
        collectLocalLambdas(found, func);
        global_const_int_values = std::move(saved_const_int_values);
    }

    for (const auto& fn : source_functions) {
        if (fn.name == func.name && clang_equalCursors(fn.cursor, found)) continue;
        if (fn.name == func.name) continue;
        auto saved_template_int_values = current_template_int_values;
        auto template_bindings = templateBindingsForSpecializedFunction(fn.cursor);
        for (const auto& [name, value] : template_bindings) {
            current_template_int_values[name] = value;
        }
        auto helper = std::make_shared<FunctionAST>(convertFunctionDecl(fn.cursor, fn.name));
        helper->struct_fields = struct_metadata.struct_fields;
        helper->struct_constructors = struct_metadata.struct_constructors;
        {
            auto saved_const_int_values = global_const_int_values;
            collectScopedConstInts(fn.cursor, cursorFileName(fn.cursor));
            collectLocalLambdas(fn.cursor, *helper);
            global_const_int_values = std::move(saved_const_int_values);
        }
        current_template_int_values = std::move(saved_template_int_values);
        func.helpers.push_back(helper);
    }

    appendPendingFunctionTemplateSpecializations(func);
    propagateLambdaCaptureDependencies(func);
    normalizeLambdaCallsInFunction(func);
    liftGlobalPortsIntoFunctions(func);

    result.function = func;
    active_struct_fields = nullptr;
    active_struct_constructors = nullptr;

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return result;
}

// --- Public API ---

NativeBuildResult buildV2ASTFromSource(const std::string& source_file,
                               const std::string& top_function,
                               const std::vector<std::string>& extra_args) {
    return buildV2ASTFromSourceImpl(source_file, nullptr, top_function, extra_args);
}

NativeBuildResult buildV2ASTFromSourceText(const std::string& source_name,
                                   const std::string& source_text,
                                   const std::string& top_function,
                                   const std::vector<std::string>& extra_args) {
    std::string name = source_name.empty() ? "/tmp/rtlzz_input.logic.cpp" : source_name;
    return buildV2ASTFromSourceImpl(name, &source_text, top_function, extra_args);
}

} // namespace pred
