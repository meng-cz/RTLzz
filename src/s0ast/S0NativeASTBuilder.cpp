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
static std::unordered_map<std::string, long long> global_const_int_values;

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

static void registerLambdaOperatorName(CXCursor lambda_cursor, const std::string& name) {
    if (name.empty()) return;
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
        std::string name = cxToStr(clang_getCursorSpelling(cursor));
        CXCursor lambda = findLambdaExpr(cursor);
        if (!clang_equalCursors(lambda, clang_getNullCursor())) {
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
        std::string sig = operatorSignature(referenced);
        auto sig_it = lambda_operator_signature_to_name.find(sig);
        if (sig_it != lambda_operator_signature_to_name.end() && !sig_it->second.empty()) return sig_it->second;
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
                ctx->functions.push_back({cxToStr(clang_getCursorSpelling(c)), c});
            }
        }
        return CXChildVisit_Continue;
    }, &ctx);
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
        return make_var(name, convertType(type));
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
            // Extract operator from tokens
            CXSourceRange range = clang_getCursorExtent(cursor);
            CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
            CXToken* tokens = nullptr;
            unsigned numTokens = 0;
            clang_tokenize(tu, range, &tokens, &numTokens);

            // Find the operator token (between LHS and RHS)
            CXSourceLocation lhsEnd = clang_getRangeEnd(clang_getCursorExtent(children[0]));
            CXSourceLocation rhsStart = clang_getRangeStart(clang_getCursorExtent(children[1]));
            unsigned lhsEndOffset = 0, rhsStartOffset = 0;
            CXFile f;
            unsigned line, col;
            clang_getSpellingLocation(lhsEnd, &f, &line, &col, &lhsEndOffset);
            clang_getSpellingLocation(rhsStart, &f, &line, &col, &rhsStartOffset);

            std::string op = "?";
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
        std::string spelling = cxToStr(clang_getCursorSpelling(cursor));
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
            if (converted && target_type.width > 0) {
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
            std::string token_callee = lambdaNameForOperatorCursor(children.front());
            if (token_callee.empty()) token_callee = lambdaNameForOperatorCursor(cursor);
            if (token_callee.empty()) token_callee = lambdaNameForOperatorCallType(cursor);
            if (!token_callee.empty() && token_callee != "operator()") {
                auto result = std::make_shared<Expr>();
                result->kind = ExprKind::Call;
                result->callee = token_callee;
                result->type = convertType(type);
                int arg_count = clang_Cursor_getNumArguments(cursor);
                for (int i = 0; i < arg_count; ++i) {
                    result->args.push_back(convertExpr(clang_Cursor_getArgument(cursor, static_cast<unsigned>(i))));
                }
                return result;
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
            std::string callee = astBaseName(receiver);
            if (!callee.empty()) {
                auto result = std::make_shared<Expr>();
                result->kind = ExprKind::Call;
                result->callee = callee;
                result->type = convertType(type);
                result->args = std::move(args);
                return result;
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
            if (range_args.empty()) {
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
        result->type = convertType(type);
        size_t arg_start = 1;
        if (spelling == "operator()") {
            result->callee.clear();
            for (size_t i = 0; i < children.size(); ++i) {
                if (firstSpellingDeep(children[i]) == "operator()") continue;
                auto candidate = convertExpr(children[i]);
                std::string name = astBaseName(candidate);
                if (!name.empty() && name != "operator()") {
                    result->callee = name;
                    arg_start = i + 1;
                    break;
                }
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
                result->args.push_back(convertExpr(
                    clang_Cursor_getArgument(cursor, static_cast<unsigned>(i))));
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
            if (firstSpellingDeep(children.front()).rfind("operator", 0) == 0) {
                auto converted = convertExpr(children.front());
                TypeInfo target_type = convertType(type);
                if (converted && converted->kind == ExprKind::FieldAccess &&
                    converted->field_name.rfind("operator", 0) == 0 &&
                    converted->struct_base) {
                    converted = converted->struct_base;
                }
                if (converted && target_type.width > 0) {
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
            std::string callee = lambdaNameForOperatorCursor(children[0]);
            if (callee.empty()) callee = lambdaNameForOperatorCursor(cursor);
            if (callee.empty()) callee = lambdaNameForOperatorCallType(cursor);
            if (!callee.empty()) {
                auto recovered = std::make_shared<Expr>();
                recovered->kind = ExprKind::Call;
                recovered->callee = callee;
                recovered->type = convertType(type);
                return recovered;
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
    auto children = getChildren(cursor);

    switch (kind) {
    case CXCursor_DeclStmt: {
        // May contain VarDecl children
        for (auto& child : children) {
            if (clang_getCursorKind(child) == CXCursor_VarDecl) {
                auto var_children = getChildren(child);
                bool is_lambda_decl = false;
                for (auto& vc : var_children) {
                    if (clang_getCursorKind(vc) == CXCursor_LambdaExpr) {
                        is_lambda_decl = true;
                        break;
                    }
                }
                if (is_lambda_decl) return nullptr;

                auto stmt = std::make_shared<Stmt>();
                stmt->kind = StmtKind::Decl;
                stmt->decl_name = cxToStr(clang_getCursorSpelling(child));
                stmt->decl_type = convertType(clang_getCursorType(child));
                if (clang_Cursor_getStorageClass(child) == CX_SC_Static) {
                    stmt->decl_type.is_static = true;
                }
                CXCursor init_expr = clang_getNullCursor();
                for (auto& vc : var_children) {
                    if (clang_getCursorKind(vc) == CXCursor_LambdaExpr) continue;
                    if (clang_isExpression(clang_getCursorKind(vc))) {
                        init_expr = vc;
                    }
                }
                if (stmt->decl_type.is_static && stmt->decl_type.is_array &&
                    !clang_Cursor_isNull(init_expr)) {
                    collectIntegerInitValues(init_expr, stmt->decl_type.init_values);
                }
                stmt->decl_default_constructed =
                    clang_Cursor_isNull(init_expr) && isVulFixedIntType(stmt->decl_type);
                if (!clang_Cursor_isNull(init_expr)) {
                    stmt->decl_init = convertExpr(init_expr);
                    if (isAggregateInitTargetType(stmt->decl_type) &&
                        isAggregateInitCursor(init_expr)) {
                        collectInitArgExprs(init_expr, stmt->decl_init_args);
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
            CXSourceRange range = clang_getCursorExtent(cursor);
            CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
            CXToken* tokens = nullptr;
            unsigned numTokens = 0;
            clang_tokenize(tu, range, &tokens, &numTokens);

            CXSourceLocation lhsEnd = clang_getRangeEnd(clang_getCursorExtent(children[0]));
            unsigned lhsEndOffset = 0;
            CXFile f; unsigned line, col;
            clang_getSpellingLocation(lhsEnd, &f, &line, &col, &lhsEndOffset);

            CXSourceLocation rhsStart = clang_getRangeStart(clang_getCursorExtent(children[1]));
            unsigned rhsStartOffset = 0;
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

static FunctionAST convertFunctionDecl(CXCursor cursor, const std::string& name) {
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

    return func;
}

static std::shared_ptr<FunctionAST> convertLambdaExpr(CXCursor lambda_cursor,
                                                      const std::string& name) {
    auto fn = std::make_shared<FunctionAST>();
    fn->name = name;
    registerLambdaOperatorName(lambda_cursor, name);
    fn->return_type = TypeInfo{"auto", 0, false};
    auto collect_nested_lambdas = [&](CXCursor compound) {
        for (auto& stmt_child : getChildren(compound)) {
            if (clang_getCursorKind(stmt_child) != CXCursor_DeclStmt) continue;
            for (auto& decl_child : getChildren(stmt_child)) {
                if (clang_getCursorKind(decl_child) != CXCursor_VarDecl) continue;
                std::string nested_name = cxToStr(clang_getCursorSpelling(decl_child));
                for (auto& init_child : getChildren(decl_child)) {
                    if (clang_getCursorKind(init_child) == CXCursor_LambdaExpr) {
                        fn->lambdas[nested_name] = convertLambdaExpr(init_child, nested_name);
                        break;
                    }
                }
            }
        }
    };
    auto lambda_children = getChildren(lambda_cursor);
    for (auto& c : lambda_children) {
        auto kind = clang_getCursorKind(c);
        if (kind == CXCursor_ParmDecl) {
            fn->params.push_back(makeParamDeclFromCursor(c));
        } else if (kind == CXCursor_CompoundStmt && fn->body.empty()) {
            fn->body = convertBlock(c);
            collect_nested_lambdas(c);
        }
    }
    auto collect_lambda_method = [&](CXCursor method) {
        TypeInfo method_return = convertType(clang_getCursorResultType(method));
        if (!method_return.name.empty() || method_return.width > 0) {
            fn->return_type = method_return;
        }
        struct LambdaMethodCtx {
            FunctionAST* fn;
        } ctx{fn.get()};
        clang_visitChildren(method, [](CXCursor mc, CXCursor, CXClientData data) {
            auto* ctx = static_cast<LambdaMethodCtx*>(data);
            auto mk = clang_getCursorKind(mc);
            if (mk == CXCursor_ParmDecl) {
                ctx->fn->params.push_back(makeParamDeclFromCursor(mc));
            } else if (mk == CXCursor_CompoundStmt && ctx->fn->body.empty()) {
                ctx->fn->body = convertBlock(mc);
            }
            return CXChildVisit_Continue;
        }, &ctx);
    };
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

    if (fn->body.empty()) {
        for (auto& c : lambda_children) {
            auto kind = clang_getCursorKind(c);
            if (kind == CXCursor_ParmDecl) {
                fn->params.push_back(makeParamDeclFromCursor(c));
            } else if (kind == CXCursor_CompoundStmt && fn->body.empty()) {
                fn->body = convertBlock(c);
                collect_nested_lambdas(c);
            } else if (kind == CXCursor_CXXMethod) {
                std::string method_name = cxToStr(clang_getCursorSpelling(c));
                if (method_name == "operator()") {
                    collect_lambda_method(c);
                }
            }
        }
    }

    for (auto& s : fn->body) {
        if (s && s->kind == StmtKind::Return && s->return_value.has_value()) {
            fn->return_type = s->return_value.value()->type;
            break;
        }
    }
    return fn;
}

static void collectLocalLambdas(CXCursor cursor, FunctionAST& func) {
    for (auto& fn_child : getChildren(cursor)) {
        if (clang_getCursorKind(fn_child) != CXCursor_CompoundStmt) continue;
        for (auto& stmt_child : getChildren(fn_child)) {
            if (clang_getCursorKind(stmt_child) != CXCursor_DeclStmt) continue;
            for (auto& decl_child : getChildren(stmt_child)) {
                if (clang_getCursorKind(decl_child) != CXCursor_VarDecl) continue;
                std::string name = cxToStr(clang_getCursorSpelling(decl_child));
                for (auto& init_child : getChildren(decl_child)) {
                    if (clang_getCursorKind(init_child) == CXCursor_LambdaExpr) {
                        auto lambda = convertLambdaExpr(init_child, name);
                        if (lambda) {
                            for (auto& [nested_name, nested_lambda] : lambda->lambdas) {
                                func.lambdas[nested_name] = nested_lambda;
                            }
                            func.lambdas[name] = lambda;
                        }
                        break;
                    }
                }
            }
        }
        break;
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
    stmt->decl_type.is_static = true;
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
        if (clang_getCursorKind(c) != CXCursor_VarDecl) return CXChildVisit_Continue;
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

static NativeBuildResult buildV2ASTFromSourceImpl(const std::string& source_file,
                                          const std::string* source_text,
                                          const std::string& top_function,
                                          const std::vector<std::string>& extra_args) {
    NativeBuildResult result;
    lambda_operator_usr_to_name.clear();
    lambda_operator_location_to_name.clear();
    lambda_operator_signature_to_name.clear();
    global_const_int_values.clear();

    CXIndex index = clang_createIndex(0, 0);

    std::vector<const char*> args;
    args.push_back("-std=c++17");
    args.push_back("-fsyntax-only");
    for (auto& a : extra_args) args.push_back(a.c_str());

    std::string parse_file = source_file;
    std::string source_buffer;
    std::string wrapper_source;
    std::vector<CXUnsavedFile> unsaved_storage;
    unsigned unsaved_count = 0;
    if (source_text) {
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

    FunctionAST func = convertFunctionDecl(found, resolved_top_function);
    func.struct_fields = struct_metadata.struct_fields;
    func.struct_constructors = struct_metadata.struct_constructors;
    for (const auto& param : func.params) {
        std::string invalid = invalidTopParamReason(param);
        if (!invalid.empty()) {
            result.error = invalid;
            active_struct_fields = nullptr;
            active_struct_constructors = nullptr;
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return result;
        }
    }
    collectLocalLambdas(found, func);

    std::vector<StmtPtr> global_static_decls;
    clang_visitChildren(root, [](CXCursor c, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto* decls = static_cast<std::vector<StmtPtr>*>(data);
        auto stmt = convertStaticArrayVarDecl(c);
        if (!stmt) stmt = convertGlobalConstScalarDecl(c);
        if (stmt) decls->push_back(stmt);
        return CXChildVisit_Continue;
    }, &global_static_decls);
    if (!global_static_decls.empty()) {
        global_static_decls.insert(global_static_decls.end(), func.body.begin(), func.body.end());
        func.body = std::move(global_static_decls);
    }

    for (const auto& fn : source_functions) {
        if (fn.name == func.name && clang_equalCursors(fn.cursor, found)) continue;
        if (fn.name == func.name) continue;
        auto helper = std::make_shared<FunctionAST>(convertFunctionDecl(fn.cursor, fn.name));
        helper->struct_fields = struct_metadata.struct_fields;
        helper->struct_constructors = struct_metadata.struct_constructors;
        func.helpers.push_back(helper);
    }

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
