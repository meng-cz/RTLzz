#include "ast/ASTBuilder.h"
#include "ast/VulOpRecognizer.h"
#include "ast/VulTypeRecognizer.h"
#include "transform/LoopUnroll.h"
#include <clang-c/Index.h>
#include <cstring>
#include <functional>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace pred {

// --- Helpers ---

static std::string cxToStr(CXString s) {
    const char* c = clang_getCString(s);
    std::string result = c ? c : "";
    clang_disposeString(s);
    return result;
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
    if (type.kind == CXType_LValueReference || type.kind == CXType_RValueReference) {
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

// Forward declarations
static ExprPtr convertExpr(CXCursor cursor);
static StmtPtr convertStmt(CXCursor cursor);
static std::vector<StmtPtr> convertChildren(CXCursor cursor);

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

static constexpr bool allowUnsafeTextFallback = false;
static std::unordered_map<std::string, std::string> lambda_operator_usr_to_name;
static std::unordered_map<std::string, std::string> lambda_operator_location_to_name;
static std::unordered_map<std::string, std::string> lambda_operator_signature_to_name;

static std::string cursorText(CXCursor cursor, bool allow_large = false) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
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

static unsigned cursorLineSpan(CXCursor cursor) {
    return sourceRangeLineSpan(clang_getCursorExtent(cursor));
}

static std::string cursorSourceSlice(CXCursor cursor, unsigned max_lines = 2, unsigned max_bytes = 512) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    CXSourceRange range = clang_getCursorExtent(cursor);
    if (sourceRangeLineSpan(range) > max_lines) return "";

    CXFile start_file = nullptr;
    CXFile end_file = nullptr;
    unsigned start_line = 0, start_col = 0, start_off = 0;
    unsigned end_line = 0, end_col = 0, end_off = 0;
    clang_getSpellingLocation(clang_getRangeStart(range), &start_file, &start_line, &start_col, &start_off);
    clang_getSpellingLocation(clang_getRangeEnd(range), &end_file, &end_line, &end_col, &end_off);
    if (!start_file || !end_file || !clang_File_isEqual(start_file, end_file)) return "";
    if (end_off <= start_off || end_off - start_off > max_bytes) return "";

    std::string file_name = cxToStr(clang_getFileName(start_file));
    std::ifstream in(file_name, std::ios::binary);
    if (!in) return "";
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (start_off >= static_cast<unsigned>(size) || end_off > static_cast<unsigned>(size)) return "";
    std::string text(end_off - start_off, '\0');
    in.seekg(start_off, std::ios::beg);
    in.read(&text[0], static_cast<std::streamsize>(text.size()));
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

static std::string cursorUsr(CXCursor cursor) {
    return cxToStr(clang_getCursorUSR(cursor));
}

static std::vector<std::string> integerTokensInBraces(const std::string& text) {
    std::vector<std::string> values;
    auto l = text.find('{');
    auto r = text.rfind('}');
    if (l == std::string::npos || r == std::string::npos || l >= r) return values;
    std::string cur;
    for (size_t i = l + 1; i < r; ++i) {
        char c = text[i];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == 'x' || c == 'X') {
            cur += c;
        } else if (!cur.empty()) {
            values.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) values.push_back(cur);
    return values;
}

static std::vector<int> arrayDimsAfterName(const std::string& text, const std::string& name) {
    std::vector<int> dims;
    size_t pos = text.find(name);
    if (pos == std::string::npos) return dims;
    pos += name.size();
    while (pos < text.size()) {
        pos = text.find('[', pos);
        if (pos == std::string::npos) break;
        size_t end = text.find(']', pos);
        if (end == std::string::npos) break;
        std::string dim = text.substr(pos + 1, end - pos - 1);
        dim.erase(std::remove_if(dim.begin(), dim.end(), [](unsigned char c) {
            return std::isspace(c);
        }), dim.end());
        if (!dim.empty() && std::all_of(dim.begin(), dim.end(), [](unsigned char c) {
                return std::isdigit(c);
            })) {
            try { dims.push_back(std::stoi(dim)); } catch (...) {}
        }
        pos = end + 1;
    }
    return dims;
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

static bool isUnsupportedOperatorReceiverCall(const ExprPtr& e) {
    return e && e->kind == ExprKind::Call &&
        e->callee.rfind("__unsupported_operator_call_receiver", 0) == 0;
}

static bool containsUnsupportedOperatorReceiverCall(const ExprPtr& e) {
    if (!e) return false;
    if (isUnsupportedOperatorReceiverCall(e)) return true;
    if (e->kind == ExprKind::Cast) return containsUnsupportedOperatorReceiverCall(e->cast_expr);
    if (e->kind == ExprKind::UnaryOp) return containsUnsupportedOperatorReceiverCall(e->operand);
    return false;
}

static std::string noArgCallNameFromText(std::string text) { // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
    auto eq = text.find('=');
    if (eq != std::string::npos) text = text.substr(eq + 1);
    auto semi = text.find(';');
    if (semi != std::string::npos) text = text.substr(0, semi);
    text.erase(0, text.find_first_not_of(" \t"));
    auto end_trim = text.find_last_not_of(" \t");
    if (end_trim == std::string::npos) return "";
    text.erase(end_trim + 1);
    auto l = text.find('(');
    auto r = text.find(')', l == std::string::npos ? 0 : l);
    if (l == std::string::npos || r == std::string::npos || r <= l) return "";
    std::string args = text.substr(l + 1, r - l - 1);
    if (args.find_first_not_of(" \t") != std::string::npos) return "";
    std::string name = text.substr(0, l);
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) {
        return std::isspace(c);
    }), name.end());
    if (name.empty() || name.rfind("operator", 0) == 0) return "";
    if (!(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_')) return "";
    if (!std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '_';
        })) {
        return "";
    }
    return name;
}

static ExprPtr parseTextArrayAccess(const std::string& text, TypeInfo type = {}); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
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

static std::string simpleMemberGetReceiver(CXCursor cursor) {
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, clang_getCursorExtent(cursor), &tokens, &numTokens);
    std::string receiver;
    if (numTokens >= 4) {
        std::string t0 = cxToStr(clang_getTokenSpelling(tu, tokens[0]));
        std::string t1 = cxToStr(clang_getTokenSpelling(tu, tokens[1]));
        std::string t2 = cxToStr(clang_getTokenSpelling(tu, tokens[2]));
        std::string t3 = cxToStr(clang_getTokenSpelling(tu, tokens[3]));
        bool ident = !t0.empty() &&
            (std::isalpha(static_cast<unsigned char>(t0.front())) || t0.front() == '_') &&
            std::all_of(t0.begin(), t0.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            });
        if (ident && t1 == "." && t2 == "get" && t3 == "(") receiver = t0;
    }
    clang_disposeTokens(tu, tokens, numTokens);
    if (receiver.empty()) {
        std::string text = cursorText(cursor, true); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
        auto dot = text.find('.');
        auto arrow = text.find("->");
        size_t pos = dot != std::string::npos ? dot : arrow;
        if (pos != std::string::npos) {
            std::string prefix = text.substr(0, pos);
            prefix.erase(std::remove_if(prefix.begin(), prefix.end(), [](unsigned char c) {
                return std::isspace(c);
            }), prefix.end());
            auto scope = prefix.find_last_of("({[;,");
            if (scope != std::string::npos) prefix = prefix.substr(scope + 1);
            bool ident = !prefix.empty() &&
                (std::isalpha(static_cast<unsigned char>(prefix.front())) || prefix.front() == '_') &&
                std::all_of(prefix.begin(), prefix.end(), [](unsigned char c) {
                    return std::isalnum(c) || c == '_';
                });
            if (ident) receiver = prefix;
        }
    }
    return receiver;
}

static std::string leadingIdentifierFromTokens(CXCursor cursor) { // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, clang_getCursorExtent(cursor), &tokens, &numTokens);
    std::string name;
    auto is_ident = [](const std::string& t) {
        return !t.empty() &&
            (std::isalpha(static_cast<unsigned char>(t.front())) || t.front() == '_') &&
            std::all_of(t.begin(), t.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            });
    };
    if (numTokens >= 4) {
        std::string t0 = cxToStr(clang_getTokenSpelling(tu, tokens[0]));
        std::string t1 = cxToStr(clang_getTokenSpelling(tu, tokens[1]));
        std::string t2 = cxToStr(clang_getTokenSpelling(tu, tokens[2]));
        std::string t3 = cxToStr(clang_getTokenSpelling(tu, tokens[3]));
        if (t0 == "(" && is_ident(t1) && t2 == ")" && t3 == "(") name = t1;
    }
    if (name.empty() && numTokens >= 2) {
        std::string t0 = cxToStr(clang_getTokenSpelling(tu, tokens[0]));
        std::string t1 = cxToStr(clang_getTokenSpelling(tu, tokens[1]));
        bool ident = is_ident(t0);
        if (ident && t1 == "(") name = t0;
    }
    clang_disposeTokens(tu, tokens, numTokens);
    return name;
}

static std::string memberReceiverFromTokens(CXCursor cursor) { // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, clang_getCursorExtent(cursor), &tokens, &numTokens);
    auto is_ident = [](const std::string& t) {
        return !t.empty() &&
            (std::isalpha(static_cast<unsigned char>(t.front())) || t.front() == '_') &&
            std::all_of(t.begin(), t.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            });
    };
    std::string receiver;
    for (unsigned i = 1; i < numTokens; ++i) {
        std::string tok = cxToStr(clang_getTokenSpelling(tu, tokens[i]));
        if (tok != "." && tok != "->") continue;
        std::string prev = cxToStr(clang_getTokenSpelling(tu, tokens[i - 1]));
        if (is_ident(prev)) {
            receiver = prev;
            break;
        }
    }
    clang_disposeTokens(tu, tokens, numTokens);
    return receiver;
}

static std::string noArgCallReceiverFromSource(CXCursor cursor) { // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
    std::string text = cursorSourceSlice(cursor, 1, 256); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
    if (text.empty()) text = cursorText(cursor, true); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
    auto paren = text.find('(');
    if (paren == std::string::npos) return "";
    size_t end = paren;
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    size_t begin = end;
    while (begin > 0) {
        char c = text[begin - 1];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) break;
        --begin;
    }
    if (begin >= end) return "";
    std::string name = text.substr(begin, end - begin);
    if (name.rfind("operator", 0) == 0) return "";
    return name;
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

static std::string leadingArrayIdentifierFromTokens(CXCursor cursor) {
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, clang_getCursorExtent(cursor), &tokens, &numTokens);
    std::string name;
    if (numTokens >= 2) {
        std::string t0 = cxToStr(clang_getTokenSpelling(tu, tokens[0]));
        std::string t1 = cxToStr(clang_getTokenSpelling(tu, tokens[1]));
        bool ident = !t0.empty() &&
            (std::isalpha(static_cast<unsigned char>(t0.front())) || t0.front() == '_') &&
            std::all_of(t0.begin(), t0.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            });
        if (ident && t1 == "[") name = t0;
    }
    clang_disposeTokens(tu, tokens, numTokens);
    return name;
}

static ExprPtr simpleTokenExpr(const std::vector<std::string>& toks, size_t begin, size_t end) {
    if (begin >= end) return make_literal("0", TypeInfo{"int", 32, true});
    int depth = 0;
    for (size_t i = end; i-- > begin;) {
        if (toks[i] == ")") ++depth;
        else if (toks[i] == "(") --depth;
        else if (depth == 0 && (toks[i] == "+" || toks[i] == "-") && i > begin) {
            return make_binary(toks[i],
                               simpleTokenExpr(toks, begin, i),
                               simpleTokenExpr(toks, i + 1, end),
                               TypeInfo{"int", 32, true});
        }
    }
    depth = 0;
    for (size_t i = end; i-- > begin;) {
        if (toks[i] == ")") ++depth;
        else if (toks[i] == "(") --depth;
        else if (depth == 0 && toks[i] == "*" && i > begin) {
            return make_binary("*",
                               simpleTokenExpr(toks, begin, i),
                               simpleTokenExpr(toks, i + 1, end),
                               TypeInfo{"int", 32, true});
        }
    }
    if (end == begin + 1) {
        const auto& tok = toks[begin];
        bool number = !tok.empty() && std::all_of(tok.begin(), tok.end(), [](unsigned char c) {
            return std::isdigit(c);
        });
        return number ? make_literal(tok, TypeInfo{"int", 32, true}) : make_var(tok, TypeInfo{"int", 32, true});
    }
    if (toks[begin] == "(" && toks[end - 1] == ")") {
        return simpleTokenExpr(toks, begin + 1, end - 1);
    }
    return make_var(toks[begin], TypeInfo{"int", 32, true});
}

static ExprPtr arrayAccessFromBracketTokens(CXCursor cursor, TypeInfo type = {}) { // UNSAFE_TEXT_FALLBACK_ALLOW: libclang operator[] bracket recovery, not source lowering
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, clang_getCursorExtent(cursor), &tokens, &numTokens);
    std::vector<std::string> toks;
    toks.reserve(numTokens);
    for (unsigned i = 0; i < numTokens; ++i) {
        toks.push_back(cxToStr(clang_getTokenSpelling(tu, tokens[i])));
    }
    clang_disposeTokens(tu, tokens, numTokens);
    if (toks.size() < 4) return nullptr;
    auto is_ident = [](const std::string& tok) {
        return !tok.empty() &&
            (std::isalpha(static_cast<unsigned char>(tok.front())) || tok.front() == '_') &&
            std::all_of(tok.begin(), tok.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            });
    };
    size_t base_pos = std::string::npos;
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        if (is_ident(toks[i]) && toks[i + 1] == "[") {
            base_pos = i;
            break;
        }
    }
    if (base_pos == std::string::npos) return nullptr;
    const std::string& base_name = toks[base_pos];
    if (base_name.empty() ||
        (!std::isalpha(static_cast<unsigned char>(base_name.front())) && base_name.front() != '_')) {
        return nullptr;
    }
    ExprPtr out = make_var(base_name);
    size_t pos = base_pos + 1;
    while (pos < toks.size() && toks[pos] == "[") {
        size_t start = pos + 1;
        int depth = 1;
        size_t end = start;
        for (; end < toks.size(); ++end) {
            if (toks[end] == "[") ++depth;
            else if (toks[end] == "]") {
                --depth;
                if (depth == 0) break;
            }
        }
        if (end >= toks.size() || end == start) return nullptr;
        out = make_array_access(out, simpleTokenExpr(toks, start, end), type);
        pos = end + 1;
    }
    return out;
}

static std::string assignmentLhsIdentifierFromTokens(CXCursor cursor) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXToken* tokens = nullptr;
    unsigned numTokens = 0;
    clang_tokenize(tu, clang_getCursorExtent(cursor), &tokens, &numTokens);
    std::string name;
    for (unsigned i = 0; i + 1 < numTokens; ++i) {
        std::string tok = cxToStr(clang_getTokenSpelling(tu, tokens[i]));
        std::string next = cxToStr(clang_getTokenSpelling(tu, tokens[i + 1]));
        bool ident = !tok.empty() &&
            (std::isalpha(static_cast<unsigned char>(tok.front())) || tok.front() == '_') &&
            std::all_of(tok.begin(), tok.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '_';
            });
        if (ident && next == "=") {
            name = tok;
            break;
        }
    }
    clang_disposeTokens(tu, tokens, numTokens);
    return name;
}

static ExprPtr tokenExpr(const std::string& tok) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    std::string t = tok;
    t.erase(std::remove_if(t.begin(), t.end(), [](unsigned char c) {
        return std::isspace(c);
    }), t.end());
    if (t.empty()) return make_literal("0", TypeInfo{"int", 32, true});
    bool number = std::all_of(t.begin(), t.end(), [](unsigned char c) {
        return std::isdigit(c);
    });
    if (number) return make_literal(t, TypeInfo{"int", 32, true});
    return make_var(t, TypeInfo{"int", 32, true});
}

static ExprPtr parseTextArrayAccess(const std::string& text, TypeInfo type) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    auto b = text.find('[');
    if (b == std::string::npos) return nullptr;
    auto paren = text.find('(');
    if (paren != std::string::npos && paren < b) return nullptr;
    std::string base_name = text.substr(0, b);
    base_name.erase(std::remove_if(base_name.begin(), base_name.end(), [](unsigned char c) {
        return std::isspace(c);
    }), base_name.end());
    if (base_name.empty()) return nullptr;
    ExprPtr e = make_var(base_name);
    size_t pos = b;
    while (pos != std::string::npos && pos < text.size()) {
        auto l = text.find('[', pos);
        if (l == std::string::npos) break;
        auto r = text.find(']', l);
        if (r == std::string::npos) break;
        std::string idx = text.substr(l + 1, r - l - 1);
        idx.erase(std::remove_if(idx.begin(), idx.end(), [](unsigned char c) {
            return std::isspace(c);
        }), idx.end());
        bool number = !idx.empty() && std::all_of(idx.begin(), idx.end(), [](unsigned char c) {
            return std::isdigit(c);
        });
        ExprPtr idx_expr = number
            ? make_literal(idx, TypeInfo{"int", 32, true})
            : make_var(idx, TypeInfo{"int", 32, true});
        e = make_array_access(e, idx_expr, type);
        pos = r + 1;
    }
    return e;
}

static bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isdigit(c);
    });
}

static ExprPtr parseBitSelectCall(const std::string& text, TypeInfo type = {}) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    auto l = text.rfind('(');
    auto r = text.rfind(')');
    if (l == std::string::npos || r == std::string::npos || l >= r) return nullptr;
    std::string base = text.substr(0, l);
    base.erase(std::remove_if(base.begin(), base.end(), [](unsigned char c) {
        return std::isspace(c);
    }), base.end());
    while (base.size() >= 2 && base.front() == '(' && base.back() == ')') {
        int depth = 0;
        bool wraps = true;
        for (size_t i = 0; i < base.size(); ++i) {
            if (base[i] == '(') ++depth;
            if (base[i] == ')') --depth;
            if (depth == 0 && i + 1 < base.size()) {
                wraps = false;
                break;
            }
        }
        if (!wraps) break;
        base = base.substr(1, base.size() - 2);
    }
    if (base.empty()) return nullptr;
    if (!std::isalpha(static_cast<unsigned char>(base.front())) && base.front() != '_') return nullptr;
    if (base.find("operator") != std::string::npos || base.find("::") != std::string::npos ||
        base.find_first_of("+-*/%<>&|^!~=,") != std::string::npos) return nullptr;

    std::vector<std::string> args;
    std::string cur;
    int depth = 0;
    for (size_t i = l + 1; i < r; ++i) {
        char c = text[i];
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        if (c == ',' && depth == 0) {
            cur.erase(std::remove_if(cur.begin(), cur.end(), [](unsigned char ch) {
                return std::isspace(ch);
            }), cur.end());
            args.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    cur.erase(std::remove_if(cur.begin(), cur.end(), [](unsigned char ch) {
        return std::isspace(ch);
    }), cur.end());
    if (!cur.empty()) args.push_back(cur);
    if ((args.size() != 1 && args.size() != 2) ||
        !std::all_of(args.begin(), args.end(), allDigits)) {
        return nullptr;
    }

    if (args.size() == 1) {
        return make_bit_select(make_var(base), std::stoi(args[0]));
    }
    int hi = std::stoi(args[0]);
    int lo = std::stoi(args[1]);
    if (hi >= lo) type.width = hi - lo + 1;
    return make_slice(make_var(base), hi, lo, type);
}

static int typeWidthFromText(const std::string& text,
                             const std::string& token) {
    int width = 0;
    return parseVulWidthName(text, token, width) ? width : 0;
}

static std::vector<int> staticRangeArgsFromType(CXType type) {
    std::string spelling = cxToStr(clang_getTypeSpelling(type));
    if (spelling.find("StaticRangeProxy") == std::string::npos &&
        spelling.find("VULStaticSliceRef") == std::string::npos) {
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

static ExprPtr makeVulConcatCall(const std::vector<ExprPtr>& args) {
    std::vector<ExprPtr> parts = args;
    return make_concat(std::move(parts));
}

static ExprPtr makeVulRepeatCall(int count, ExprPtr value) {
    return make_repeat(std::move(value), count);
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

static bool isSimpleAssignmentLhsText(const std::string& s) {
    if (isSimpleIdentifierText(s)) return true;
    if (s.size() > 1 && s.front() == '*') {
        return isSimpleIdentifierText(s.substr(1));
    }
    std::string call_text = s;
    if (call_text.size() > 4 && call_text.front() == '(') {
        auto close_base = call_text.find(")(");
        if (close_base != std::string::npos) {
            std::string base = call_text.substr(1, close_base - 1);
            if (isSimpleIdentifierText(base)) {
                call_text = base + call_text.substr(close_base + 1);
            }
        }
    }
    auto lp = call_text.find('(');
    auto rp = call_text.rfind(')');
    if (lp != std::string::npos && rp == call_text.size() - 1 && lp > 0) {
        if (!isSimpleIdentifierText(call_text.substr(0, lp))) return false;
        std::string args = call_text.substr(lp + 1, rp - lp - 1);
        if (args.empty()) return false;
        size_t start = 0;
        int count = 0;
        while (start <= args.size()) {
            size_t comma = args.find(',', start);
            std::string one = args.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (one.empty() || !std::all_of(one.begin(), one.end(), [](unsigned char c) {
                    return std::isdigit(c);
                })) {
                return false;
            }
            ++count;
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return count == 1 || count == 2;
    }
    return false;
}

static StmtPtr parseSimpleVarAssignment(CXCursor cursor) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    std::string text = compactText(cursorSourceSlice(cursor, 1, 256)); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    if (text.empty()) text = compactText(cursorText(cursor)); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    if (!text.empty() && text.back() == ';') text.pop_back();
    auto eq = text.find('=');
    if (eq == std::string::npos || text.find('=', eq + 1) != std::string::npos) return nullptr;
    std::string lhs = text.substr(0, eq);
    std::string rhs = text.substr(eq + 1);
    if (!isSimpleIdentifierText(lhs) || !isSimpleIdentifierText(rhs)) return nullptr;
    return makeAssignStmtAst(make_var(lhs, TypeInfo{}), make_var(rhs, TypeInfo{}));
}

static std::string lhsTextBeforeAssign(CXCursor cursor) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    std::string text = cursorSourceSlice(cursor, 1, 512); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    if (text.empty()) text = cursorText(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
    auto eq = text.find('=');
    if (eq == std::string::npos) return "";
    std::string lhs = text.substr(0, eq);
    if (lhs.find('\n') != std::string::npos || containsAny(lhs, {"[&]", "auto", "->", "operator"})) return "";
    return lhs;
}

static std::string firstParenArgText(const std::string& text) {
    auto l = text.find('(');
    auto r = text.rfind(')');
    if (l == std::string::npos || r == std::string::npos || l >= r) return "";
    return text.substr(l + 1, r - l - 1);
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

static ParamDirection inferParamDirection(const ParamDecl& p,
                                          const std::unordered_set<std::string>& read,
                                          const std::unordered_set<std::string>& written) {
    if (p.passing == ParamPassingKind::Value || p.passing == ParamPassingKind::ConstRef ||
        p.is_const) {
        return ParamDirection::Input;
    }
    bool is_read = read.count(p.name) > 0;
    bool is_written = written.count(p.name) > 0;
    if (is_written && is_read) return ParamDirection::InOut;
    if (is_written) return ParamDirection::Output;
    if (!is_read && isMutableParamPassing(p.passing)) return ParamDirection::Output;
    return ParamDirection::Input;
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
        std::string text = cursorText(c); // UNSAFE_TEXT_FALLBACK_ALLOW: diagnostics only, not lowering.
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

static ExprPtr convertExpr(CXCursor cursor) {
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

    case CXCursor_DeclRefExpr:
    case CXCursor_MemberRefExpr: {
        if (kind == CXCursor_MemberRefExpr && !children.empty()) {
            auto base = convertExpr(children[0]);
            std::string field = cxToStr(clang_getCursorSpelling(cursor));
            TypeInfo field_type;
            CXCursor referenced = clang_getCursorReferenced(cursor);
            CXCursorKind referenced_kind = clang_Cursor_isNull(referenced)
                ? CXCursor_InvalidFile
                : clang_getCursorKind(referenced);
            const bool callable_member =
                referenced_kind == CXCursor_CXXMethod ||
                referenced_kind == CXCursor_FunctionDecl ||
                referenced_kind == CXCursor_FunctionTemplate ||
                referenced_kind == CXCursor_Constructor ||
                referenced_kind == CXCursor_ConversionFunction;
            if (!callable_member &&
                field != "setnext" && field != "call" && field != "cat" &&
                field != "repeat" && field != "reduce_or" && field != "reduce_and" &&
                field != "reduce_xor" && field != "range_at" && field != "bit_at" &&
                field != "sint" &&
                field != "get" && field != "operator()" &&
                field.rfind("operator", 0) != 0) {
                field_type = convertType(type);
            }
            return make_field_access(base, field, field_type);
        }
        if (allowUnsafeTextFallback && kind == CXCursor_MemberRefExpr) {
            std::string text = cursorText(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
            std::string field = cxToStr(clang_getCursorSpelling(cursor));
            auto arrow = text.find("->");
            auto dot = text.find('.');
            auto pos = arrow != std::string::npos ? arrow : dot;
            if (pos != std::string::npos) {
                std::string base_name = text.substr(0, pos);
                base_name.erase(std::remove_if(base_name.begin(), base_name.end(), [](unsigned char c) {
                    return std::isspace(c);
                }), base_name.end());
                return make_field_access(make_var(base_name), field, convertType(type));
            }
        }
        std::string name = cxToStr(clang_getCursorSpelling(cursor));
        return make_var(name, convertType(type));
    }

    case CXCursor_ArraySubscriptExpr: {
        if (children.size() >= 2) {
            auto base = convertExpr(children[0]);
            auto idx = convertExpr(children[1]);
            TypeInfo elem_type = convertType(type);
            if (containsUnsupportedOperatorReceiverCall(base)) {
                if (auto parsed = arrayAccessFromBracketTokens(cursor, elem_type)) { // UNSAFE_TEXT_FALLBACK_ALLOW: libclang operator[] bracket recovery, not source lowering
                    return parsed;
                }
            }
            std::string text = allowUnsafeTextFallback ? cursorText(cursor) : ""; // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
            auto bracket = text.find('[');
            if (bracket != std::string::npos) {
                std::string head = text.substr(0, bracket);
                auto arrow = head.find("->");
                auto dot = head.find('.');
                auto pos = arrow != std::string::npos ? arrow : dot;
                if (pos != std::string::npos) {
                    std::string base_name = head.substr(0, pos);
                    std::string field = head.substr(pos + (arrow != std::string::npos ? 2 : 1));
                    base_name.erase(std::remove_if(base_name.begin(), base_name.end(), [](unsigned char c) { return std::isspace(c); }), base_name.end());
                    field.erase(std::remove_if(field.begin(), field.end(), [](unsigned char c) { return std::isspace(c); }), field.end());
                    if (!base_name.empty() && !field.empty()) {
                        base = make_field_access(make_var(base_name), field, base ? base->type : TypeInfo{});
                    }
                }
            }
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
            if (cursorMentionsToken(cursor, "IntSignedView") ||
                cursorMentionsToken(cursor, "sint")) {
                forceSignedView(lhs);
                forceSignedView(rhs);
            }
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
        std::string first_child_spelling;
        if (!children.empty()) {
            first_child_spelling = firstSpellingDeep(children[0]);
            if (first_child_spelling.rfind("operator", 0) == 0) {
                spelling = first_child_spelling;
            } else if (first_child_spelling == "setnext" || first_child_spelling == "get" ||
                       first_child_spelling == "call" ||
                       first_child_spelling == "output") {
                spelling = first_child_spelling;
            }
        }
        VulCallInfo vul_call = recognizeVulCall(cursor, children, spelling, first_child_spelling);
        ExprPtr first_call_child;
        auto get_first_call_child = [&]() -> ExprPtr {
            if (!first_call_child && !children.empty()) first_call_child = convertExpr(children.front());
            return first_call_child;
        };
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
            std::string token_callee = leadingIdentifierFromTokens(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
            if (token_callee.empty()) token_callee = lambdaNameForOperatorCursor(children.front());
            if (token_callee.empty()) token_callee = lambdaNameForOperatorCursor(cursor);
            if (token_callee.empty()) token_callee = lambdaNameForOperatorCallType(cursor);
            if (token_callee.empty()) token_callee = noArgCallReceiverFromSource(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
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
            auto result = std::make_shared<Expr>();
            result->kind = ExprKind::Call;
            result->callee = "__unsupported_operator_call_receiver";
            result->type = convertType(type);
            return result;
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
            bool operator_uses_signed_view =
                cursorMentionsToken(cursor, "IntSignedView") ||
                cursorMentionsToken(cursor, "sint");
            if ((op == "+" || op == "-" || op == "*" || op == "&" || op == "|" || op == "^" ||
                 op == "<<" || op == ">>" || op == "==" || op == "!=" || op == "<" ||
                 op == "<=" || op == ">" || op == ">=")) {
                if (call_args.size() >= 2) {
                    auto lhs = convertExpr(call_args[0]);
                    auto rhs = convertExpr(call_args[1]);
                    markSignedViewIfCursorMentions(lhs, call_args[0]);
                    markSignedViewIfCursorMentions(rhs, call_args[1]);
                    if (operator_uses_signed_view) {
                        forceSignedView(lhs);
                        forceSignedView(rhs);
                    }
                    return make_binary(op, lhs, rhs, convertType(type));
                }
                auto first = get_first_call_child();
                if (first && first->kind == ExprKind::FieldAccess &&
                first->field_name == spelling && first->struct_base &&
                children.size() >= 2) {
                auto rhs = convertExpr(children.back());
                markSignedViewIfCursorMentions(rhs, children.back());
                if (operator_uses_signed_view) {
                    first->struct_base = unwrapSignedViewMemberAccess(first->struct_base);
                    forceSignedView(first->struct_base);
                    forceSignedView(rhs);
                }
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
                    if (operator_uses_signed_view) {
                        forceSignedView(lhs);
                        forceSignedView(rhs);
                    }
                    return make_binary(op, lhs, rhs, convertType(type));
                }
            }
            if (children.size() >= 2 && first_child_spelling.rfind("operator", 0) != 0 &&
                first_child_spelling != spelling) {
                auto lhs = convertExpr(children[children.size() - 2]);
                auto rhs = convertExpr(children[children.size() - 1]);
                    markSignedViewIfCursorMentions(lhs, children[children.size() - 2]);
                    markSignedViewIfCursorMentions(rhs, children[children.size() - 1]);
                    if (operator_uses_signed_view) {
                        forceSignedView(lhs);
                        forceSignedView(rhs);
                    }
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
            std::string token_callee = leadingIdentifierFromTokens(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
            size_t receiver_index = children.size() >= 2 ? 1 : 0;
            auto receiver = children.size() > receiver_index ? convertExpr(children[receiver_index]) : nullptr;
            size_t arg_start = receiver_index + 1;
            bool receiver_from_token = false;
            if (!token_callee.empty()) {
                std::string receiver_name = astBaseName(receiver);
                if (receiver_name != token_callee) {
                    receiver = make_var(token_callee, TypeInfo{});
                    arg_start = children.empty() ? 0 : 1;
                    receiver_from_token = true;
                }
            }
            std::vector<ExprPtr> args;
            for (size_t i = arg_start; i < children.size(); ++i) args.push_back(convertExpr(children[i]));
            if (receiver_from_token) {
                std::vector<ExprPtr> hw_args;
                int arg_count = clang_Cursor_getNumArguments(cursor);
                for (int i = 0; i < arg_count; ++i) {
                    hw_args.push_back(convertExpr(clang_Cursor_getArgument(cursor, static_cast<unsigned>(i))));
                }
                if (hw_args.empty()) {
                    for (auto& child : children) hw_args.push_back(convertExpr(child));
                }
                if (!hw_args.empty() && astBaseName(hw_args.front()) == "operator()") {
                    std::vector<ExprPtr> child_args;
                    for (size_t i = 0; i < children.size(); ++i) {
                        if (i == 0 && firstSpellingDeep(children[i]) == "operator()") continue;
                        child_args.push_back(convertExpr(children[i]));
                    }
                    if (!child_args.empty()) hw_args = std::move(child_args);
                }
                ExprPtr hw_receiver;
                if (!hw_args.empty() && !astBaseName(hw_args.front()).empty()) {
                    hw_receiver = hw_args.front();
                    hw_args.erase(hw_args.begin());
                } else if (!hw_args.empty() && astBaseName(hw_args.front()) == token_callee) {
                    hw_receiver = hw_args.front();
                    hw_args.erase(hw_args.begin());
                }
                if (hw_args.size() == 1 || hw_args.size() == 2) {
                    std::optional<TypeInfo> receiver_type;
                    if (hw_receiver && hw_receiver->type.is_hw_int && hw_receiver->type.width > 0) {
                        receiver_type = hw_receiver->type;
                    }
                    if (receiver_type && receiver_type->is_hw_int && receiver_type->width > 0) {
                        if (!hw_receiver) hw_receiver = make_var(token_callee, *receiver_type);
                        auto result = std::make_shared<Expr>();
                        result->kind = ExprKind::Call;
                        result->callee = hw_args.size() == 1 ? "__bit" : "__slice";
                        result->type = convertType(type);
                        result->args.push_back(hw_receiver);
                        for (auto& a : hw_args) result->args.push_back(a);
                        return result;
                    }
                }
            }
            if (!receiver_from_token && (args.size() == 1 || args.size() == 2) && receiver && receiver->type.width > 0) {
                auto result = std::make_shared<Expr>();
                result->kind = ExprKind::Call;
                result->callee = args.size() == 1 ? "__bit" : "__slice";
                result->type = convertType(type);
                result->args.push_back(receiver);
                for (auto& a : args) result->args.push_back(a);
                return result;
            }
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
        if (vul_call.kind == VulCallKind::RegProxyGet || spelling == "get") {
            if (auto get_receiver = simpleMemberGetReceiver(cursor); !get_receiver.empty()) {
                return make_var(get_receiver, convertType(type));
            }
        }
        if (vul_call.kind == VulCallKind::SetNext) {
            auto result = std::make_shared<Expr>();
            result->kind = ExprKind::Call;
            result->callee = "__vul_setnext";
            result->intrinsic = IntrinsicKind::RegProxySetNext;
            result->type = convertType(type);
            if (!children.empty()) result->args.push_back(convertExpr(children.front()));
            int port = vul_call.template_value.value_or(-1);
            if (port < 0) {
                port = templateArgIntFromTokens(cursor, "setnext", -1);
            }
            result->args.push_back(make_literal(std::to_string(port), TypeInfo{"int", 32, true}));
            for (size_t i = 1; i < children.size(); ++i) {
                result->args.push_back(convertExpr(children[i]));
            }
            return result;
        }
        if (spelling == "array" && children.size() == 1) {
            return convertExpr(children.back());
        }
        if (vul_call.kind == VulCallKind::RegProxyGet && !children.empty()) {
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess &&
                receiver->field_name == "get" && receiver->struct_base) {
                auto out = receiver->struct_base;
                out->type = convertType(type);
                return out;
            }
        }
        if (vul_call.kind == VulCallKind::SignedView && !children.empty()) {
            ExprPtr receiver;
            if (vul_call.has_receiver) {
                receiver = convertExpr(vul_call.receiver_cursor);
            } else {
                receiver = convertExpr(children.front());
            }
            receiver = unwrapSignedViewMemberAccess(receiver);
            if (receiver) {
                receiver->type.is_signed = true;
                if (!receiver->type.is_hw_int && receiver->type.width > 0) receiver->type.is_hw_int = true;
                receiver->type.hw_kind = "signed_view";
                receiver->type.name = "IntSignedView<" + std::to_string(receiver->type.width) + ">";
                return receiver;
            }
        }
        if (vul_call.kind == VulCallKind::At) {
            auto result = std::make_shared<Expr>();
            result->kind = ExprKind::Call;
            result->callee = "__slice";
            std::vector<int> range_args = vul_call.template_values;
            if (range_args.size() != 2) {
                range_args = staticRangeArgsFromType(type);
            }
            if (range_args.size() != 2) {
                result->callee = "__unsupported_at";
                result->type = convertType(type);
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
                } else if (receiver && receiver->kind == ExprKind::FieldAccess &&
                           receiver->field_name == "at" && !receiver->struct_base) {
                    std::string receiver_name = memberReceiverFromTokens(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
                    if (!receiver_name.empty()) receiver = make_var(receiver_name);
                }
            }
            if (!receiver) {
                result->callee = "__unsupported_at_receiver";
                result->type = convertType(type);
                return result;
            }
            result->type = make_hw_type(receiver->type.is_signed ? "Int" : "UInt",
                                        hi >= lo ? hi - lo + 1 : 0,
                                        receiver->type.is_signed);
            result->args.push_back(receiver);
            result->args.push_back(make_literal(std::to_string(hi), TypeInfo{"int", 32, true}));
            result->args.push_back(make_literal(std::to_string(lo), TypeInfo{"int", 32, true}));
            return result;
        }
        if (vul_call.kind == VulCallKind::Output || vul_call.kind == VulCallKind::ReqHelperCall) {
            bool is_req_output = vul_call.kind == VulCallKind::Output;
            ExprPtr receiver;
            if (!children.empty()) {
                receiver = convertExpr(children.front());
                if (receiver && receiver->kind == ExprKind::FieldAccess &&
                    receiver->field_name == "call" &&
                    receiver->struct_base && receiver->struct_base->kind == ExprKind::VarRef) {
                    is_req_output = true;
                }
            }
            if (is_req_output) {
                auto result = std::make_shared<Expr>();
                result->kind = ExprKind::Call;
                result->callee = "__vul_output";
                result->intrinsic = IntrinsicKind::ReqHelperOutput;
                result->type = convertType(type);
                if (receiver && receiver->kind == ExprKind::FieldAccess &&
                    receiver->field_name == "call" &&
                    receiver->struct_base && receiver->struct_base->kind == ExprKind::VarRef) {
                    result->args.push_back(receiver->struct_base);
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
        }
        if (cursorLineSpan(cursor) > 4) {
            auto result = std::make_shared<Expr>();
            result->kind = ExprKind::Call;
            result->callee = spelling.empty() ? "__unsupported_large_call" : spelling;
            result->type = convertType(type);
            size_t arg_start = 1;
            if (!children.empty() && clang_getCursorKind(children.front()) == CXCursor_MemberRefExpr) {
                result->args.push_back(convertExpr(children.front()));
            }
            for (size_t i = arg_start; i < children.size(); ++i) {
                result->args.push_back(convertExpr(children[i]));
            }
            return result;
        }
        std::string text;
        if (allowUnsafeTextFallback) {
            text = cursorSourceSlice(cursor, 2, 1024); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
            if (text.empty()) text = cursorText(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
        }
        if ((spelling.empty() || spelling == "operator()") && children.size() >= 2) {
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess &&
                receiver->field_name == "operator()") {
                std::vector<ExprPtr> args;
                for (size_t i = 1; i < children.size(); ++i) args.push_back(convertExpr(children[i]));
                auto result = std::make_shared<Expr>();
                result->kind = ExprKind::Call;
                result->callee = args.size() == 1 ? "__bit" : "__slice";
                result->type = convertType(type);
                result->args.push_back(receiver->struct_base);
                for (auto& a : args) result->args.push_back(a);
                return result;
            }
        }
        if (spelling.empty() || spelling == "operator()") {
            auto p = text.find('(');
            if (p != std::string::npos) {
                spelling = text.substr(0, p);
                spelling.erase(std::remove_if(spelling.begin(), spelling.end(), [](unsigned char c) {
                    return std::isspace(c);
                }), spelling.end());
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
            if ((base && base->kind == ExprKind::VarRef && base->var_name == "operator[]") ||
                containsUnsupportedOperatorReceiverCall(base) ||
                (base && astBaseName(base).empty())) {
                if (auto parsed = arrayAccessFromBracketTokens(cursor, convertType(type))) { // UNSAFE_TEXT_FALLBACK_ALLOW: libclang operator[] bracket recovery, not source lowering
                    return parsed;
                }
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
        if (vul_call.kind == VulCallKind::RangeAt || vul_call.kind == VulCallKind::BitAt ||
            spelling.find("range_at") != std::string::npos ||
            spelling.find("bit_at") != std::string::npos) {
            auto result = std::make_shared<Expr>();
            result->kind = ExprKind::Call;
            bool is_bit_at = vul_call.kind == VulCallKind::BitAt ||
                spelling.find("bit_at") != std::string::npos;
            result->callee = is_bit_at ? "__dynamic_bit_at" : "__dynamic_range_at";
            result->intrinsic = is_bit_at ? IntrinsicKind::DynamicBitAt : IntrinsicKind::DynamicRangeAt;
            result->type = is_bit_at ? make_hw_type("bool", 1, false) : convertType(type);
            if (!is_bit_at && result->type.width <= 0 && vul_call.template_value.has_value()) {
                result->type = make_hw_type("UInt", *vul_call.template_value, false);
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
        if ((spelling == "UInt" || spelling == "Int" ||
             text.rfind("UInt<", 0) == 0 || text.rfind("Int<", 0) == 0) && !children.empty()) {
            auto inner = convertExpr(children.back());
            TypeInfo target = convertType(type);
            int text_width = typeWidthFromText(text, "UInt");
            if (text_width <= 0) text_width = typeWidthFromText(text, "Int");
            if (target.width <= 0 && text_width > 0) {
                target.width = text_width;
                target.name = text.find("UInt") != std::string::npos
                    ? "UInt<" + std::to_string(text_width) + ">"
                    : "Int<" + std::to_string(text_width) + ">";
                target.is_signed = false;
                target.is_hw_int = true;
                target.hw_kind = target.name.rfind("UInt<", 0) == 0 ? "UInt" : "Int";
            }
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
                return makeVulConcatCall(args);
            }
        }
        if (vul_call.kind == VulCallKind::Cat && children.size() > 1) {
            std::vector<ExprPtr> args;
            size_t start = 1;
            if (spelling == "Cat") start = children.size() > 0 ? 1 : 0;
            for (size_t i = start; i < children.size(); ++i) {
                args.push_back(convertExpr(children[i]));
            }
            return makeVulConcatCall(args);
        }
        if (vul_call.kind == VulCallKind::Repeat && !children.empty()) {
            int count = vul_call.template_value.value_or(templateArgInt(cursor, 0, 0));
            if (count <= 0) count = templateArgIntFromTokens(cursor, "repeat", -1);
            if (children.size() >= 1) {
                auto receiver = convertExpr(children.front());
                if (receiver && receiver->kind == ExprKind::FieldAccess &&
                    receiver->field_name == "repeat") {
                    return makeVulRepeatCall(count, receiver->struct_base);
                }
            }
            if (count <= 0 && children.size() >= 3) {
                auto c = convertExpr(children[1]);
                if (c && c->kind == ExprKind::Literal) {
                    try { count = std::stoi(c->literal_value, nullptr, 0); } catch (...) {}
                }
                return makeVulRepeatCall(count, convertExpr(children[2]));
            }
            return makeVulRepeatCall(count, convertExpr(children.back()));
        }
        if (vul_call.kind == VulCallKind::ReduceOr && !children.empty()) {
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess && receiver->field_name == "reduce_or") {
                return make_reduce(ExprKind::ReduceOr, receiver->struct_base);
            }
        }
        if (vul_call.kind == VulCallKind::ReduceAnd && !children.empty()) {
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess && receiver->field_name == "reduce_and") {
                return make_reduce(ExprKind::ReduceAnd, receiver->struct_base);
            }
        }
        if (vul_call.kind == VulCallKind::ReduceXor && !children.empty()) {
            auto receiver = convertExpr(children.front());
            if (receiver && receiver->kind == ExprKind::FieldAccess && receiver->field_name == "reduce_xor") {
                return make_reduce(ExprKind::ReduceXor, receiver->struct_base);
            }
        }
        if (spelling.rfind("operator", 0) == 0) {
            std::string compact_text = compactText(text);
            if (allowUnsafeTextFallback &&
                compact_text.rfind("operator", 0) != 0 &&
                compact_text.find_first_of("+-*&|^<>") != std::string::npos) {
                auto parsed = tokenExpr(text); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
                parsed->type = convertType(type);
                return parsed;
            }
            std::string op = spelling.substr(std::string("operator").size());
            op.erase(std::remove_if(op.begin(), op.end(), [](unsigned char c) {
                return std::isspace(c);
            }), op.end());
            if (allowUnsafeTextFallback &&
                children.size() == 2 &&
                cxToStr(clang_getCursorSpelling(children[0])).rfind("operator", 0) == 0) {
                auto parsed = tokenExpr(text); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
                parsed->type = convertType(type);
                return parsed;
            }
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
                target = make_hw_type("UInt", *vul_call.template_value, false);
            }
            if (vul_call.kind == VulCallKind::Trunc) {
                return make_trunc(inner, target.width, target.is_signed);
            }
            return make_zext(inner, target.width);
        }
        if (allowUnsafeTextFallback) {
            if (auto bit_select = parseBitSelectCall(text, convertType(type))) { // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
                return bit_select;
            }
            if (allowUnsafeTextFallback && text.find('[') != std::string::npos) {
                auto arr = parseTextArrayAccess(text, convertType(type)); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
                if (arr) return arr;
            }
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
                result->callee = "__unsupported_operator_call_receiver";
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
            std::string receiver_name = memberReceiverFromTokens(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
            if (!receiver_name.empty() && receiver_name != result->args.front()->field_name) {
                result->args.front()->struct_base = make_var(receiver_name);
            }
        }
        if (result->args.empty() &&
            (spelling == "readdata" || spelling == "readreq" || spelling == "write" ||
             spelling == "front" || spelling == "enqready" || spelling == "deqvalid" ||
             spelling == "deqnext" || spelling == "clrnext" || spelling == "enqnext")) {
            std::string receiver_name = memberReceiverFromTokens(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
            if (!receiver_name.empty() && receiver_name != spelling) {
                result->args.push_back(make_field_access(make_var(receiver_name), spelling));
            }
        }
        // Use libclang's explicit argument API. Child[0] is not uniformly a
        // callee: for CXXConstructExpr it can be the first constructor
        // argument, and skipping it loses copy initialization such as
        // `TagEntry x = proxy.readdata<0>()`.
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
            if (callee.empty()) callee = noArgCallReceiverFromSource(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
            if (!callee.empty()) {
                auto recovered = std::make_shared<Expr>();
                recovered->kind = ExprKind::Call;
                recovered->callee = callee;
                recovered->type = convertType(type);
                return recovered;
            }
            auto unsupported = std::make_shared<Expr>();
            unsupported->kind = ExprKind::Call;
            unsupported->callee = "__unsupported_operator_call_receiver";
            unsupported->type = convertType(type);
            return unsupported;
        }
        break;
    }
    }

    auto unsupported = std::make_shared<Expr>();
    unsupported->kind = ExprKind::Call;
    unsupported->callee = "__unsupported_expr";
    unsupported->literal_value = cursorLocation(cursor);
    unsupported->type = convertType(type);
    return unsupported;
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

static bool isAliasCarrierType(const TypeInfo& type) {
    if (!type.struct_name.empty()) return true;
    return type.struct_name.find("__RegProxy") != std::string::npos ||
           type.struct_name.find("__ReqHelper") != std::string::npos ||
           type.name.find("__RegProxy") != std::string::npos ||
           type.name.find("__ReqHelper") != std::string::npos;
}

static bool isVulFixedIntType(const TypeInfo& type) {
    return type.width > 0 &&
           (type.hw_kind == "Int" || type.hw_kind == "UInt" ||
            type.hw_kind == "signed_view" || type.name.rfind("Int<", 0) == 0 ||
            type.name.rfind("UInt<", 0) == 0);
}

static void collectInitArgExprs(CXCursor cursor, std::vector<ExprPtr>& out, int depth = 0) {
    if (depth > 3) return;
    auto children = getChildren(cursor);
    if (children.empty()) {
        if (clang_isExpression(clang_getCursorKind(cursor))) {
            out.push_back(convertExpr(cursor));
        }
        return;
    }

    auto cursor_kind = clang_getCursorKind(cursor);
    if (cursor_kind == CXCursor_CallExpr || cursor_kind == CXCursor_InitListExpr) {
        for (auto& child : children) {
            auto kind = clang_getCursorKind(child);
            if (kind == CXCursor_TypeRef || kind == CXCursor_TemplateRef ||
                kind == CXCursor_Constructor || kind == CXCursor_CXXMethod ||
                kind == CXCursor_FunctionDecl) {
                continue;
            }
            if (clang_isExpression(kind)) {
                out.push_back(convertExpr(child));
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
            out.push_back(convertExpr(child));
            had_direct_expr = true;
        }
    }
    if (had_direct_expr) return;
    for (auto& child : children) collectInitArgExprs(child, out, depth + 1);
}

static StmtPtr convertStmt(CXCursor cursor) {
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
                auto init_values = integerTokensInBraces(cursorText(child, true)); // UNSAFE_TEXT_FALLBACK_ALLOW: static lookup-table initializer extraction.
                if (!init_values.empty()) {
                    stmt->decl_type.init_values = std::move(init_values);
                }
                std::string decl_text = cursorText(child, true); // UNSAFE_TEXT_FALLBACK_ALLOW: declaration initializer presence check.
                bool has_decl_initializer = decl_text.find('=') != std::string::npos ||
                                            decl_text.find('{') != std::string::npos;
                stmt->decl_default_constructed =
                    !has_decl_initializer && isVulFixedIntType(stmt->decl_type);
                CXCursor init_expr = clang_getNullCursor();
                for (auto& vc : var_children) {
                    if (clang_getCursorKind(vc) == CXCursor_LambdaExpr) continue;
                    if (has_decl_initializer && clang_isExpression(clang_getCursorKind(vc)) && stmt->decl_type.init_values.empty()) {
                        init_expr = vc;
                    }
                }
                if (!clang_Cursor_isNull(init_expr)) {
                    stmt->decl_init = convertExpr(init_expr);
                    if (stmt->decl_init.has_value() &&
                        containsUnsupportedOperatorReceiverCall(stmt->decl_init.value())) {
                        std::string recovered = noArgCallNameFromText(decl_text); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
                        if (!recovered.empty()) {
                            auto call = std::make_shared<Expr>();
                            call->kind = ExprKind::Call;
                            call->callee = recovered;
                            call->type = stmt->decl_type;
                            stmt->decl_init = call;
                        }
                    }
                    if (isAliasCarrierType(stmt->decl_type)) {
                        collectInitArgExprs(init_expr, stmt->decl_init_args);
                    }
                } else if (isAliasCarrierType(stmt->decl_type)) {
                    for (auto& vc : var_children) {
                        collectInitArgExprs(vc, stmt->decl_init_args);
                    }
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
        if (containsUnsupportedOperatorReceiverCall(stmt->expr_stmt)) {
            std::string recovered = noArgCallNameFromText(cursorText(cursor, true)); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
            if (!recovered.empty()) {
                auto call = std::make_shared<Expr>();
                call->kind = ExprKind::Call;
                call->callee = recovered;
                call->type = TypeInfo{"void", 0, false};
                stmt->expr_stmt = call;
            }
        }
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
                std::string lhs_text = allowUnsafeTextFallback ? compactText(lhsTextBeforeAssign(cursor)) : ""; // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
                if (allowUnsafeTextFallback && isSimpleAssignmentLhsText(lhs_text)) {
                    stmt->assign_target = tokenExpr(lhs_text); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled by allowUnsafeTextFallback.
                } else {
                    size_t lhs_index = children.size() >= 2 ? children.size() - 2 : 0;
                    stmt->assign_target = convertExpr(children[lhs_index]);
                    if (allowUnsafeTextFallback &&
                        stmt->assign_target && stmt->assign_target->kind == ExprKind::VarRef &&
                        stmt->assign_target->var_name == "operator=") {
                        std::string lhs_name = assignmentLhsIdentifierFromTokens(cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
                        if (!lhs_name.empty()) stmt->assign_target = make_var(lhs_name, convertType(clang_getCursorType(children[lhs_index])));
                    }
                }
            }
            stmt->assign_value = convertExpr(children.back());
            return stmt;
        }
        if (allowUnsafeTextFallback) {
            if (auto simple_assign = parseSimpleVarAssignment(cursor)) return simple_assign; // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
        }
        auto stmt = std::make_shared<Stmt>();
        stmt->kind = StmtKind::ExprStmt;
        stmt->expr_stmt = convertExpr(cursor);
        if (containsUnsupportedOperatorReceiverCall(stmt->expr_stmt)) {
            std::string recovered = noArgCallNameFromText(cursorText(cursor, true)); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
            if (!recovered.empty()) {
                auto call = std::make_shared<Expr>();
                call->kind = ExprKind::Call;
                call->callee = recovered;
                call->type = TypeInfo{"void", 0, false};
                stmt->expr_stmt = call;
            }
        }
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
            if (stmt->return_value.has_value() &&
                containsUnsupportedOperatorReceiverCall(stmt->return_value.value())) {
                std::string text = cursorText(cursor, true); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
                auto ret = text.find("return");
                if (ret != std::string::npos) text = text.substr(ret + std::string("return").size());
                std::string recovered = noArgCallNameFromText(text); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
                if (!recovered.empty()) {
                    auto call = std::make_shared<Expr>();
                    call->kind = ExprKind::Call;
                    call->callee = recovered;
                    call->type = stmt->return_value.value()->type;
                    stmt->return_value = call;
                }
            }
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
        if (allowUnsafeTextFallback) {
            if (auto simple_assign = parseSimpleVarAssignment(cursor)) return simple_assign; // UNSAFE_TEXT_FALLBACK_ALLOW: disabled helper only, guarded by allowUnsafeTextFallback=false
        }
        // Try as expression statement
        if (clang_isExpression(kind)) {
            auto stmt = std::make_shared<Stmt>();
            stmt->kind = StmtKind::ExprStmt;
            stmt->expr_stmt = convertExpr(cursor);
            if (containsUnsupportedOperatorReceiverCall(stmt->expr_stmt)) {
                std::string recovered = noArgCallNameFromText(cursorText(cursor, true)); // UNSAFE_TEXT_FALLBACK_ALLOW: libclang hidden operator receiver recovery, not source lowering
                if (!recovered.empty()) {
                    auto call = std::make_shared<Expr>();
                    call->kind = ExprKind::Call;
                    call->callee = recovered;
                    call->type = TypeInfo{"void", 0, false};
                    stmt->expr_stmt = call;
                }
            }
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

static FunctionAST convertFunctionDecl(CXCursor cursor, const std::string& name) {
    FunctionAST func;
    func.name = name;
    func.return_type = convertType(clang_getCursorResultType(cursor));

    int numParams = clang_Cursor_getNumArguments(cursor);
    for (int i = 0; i < numParams; ++i) {
        CXCursor param = clang_Cursor_getArgument(cursor, i);
        ParamDecl pd;
        pd.name = cxToStr(clang_getCursorSpelling(param));
        CXType pt = clang_getCursorType(param);
        pd.type = convertType(pt);
        pd.passing = classifyParamPassing(pt);
        pd.is_const = pd.type.is_const;
        pd.is_pointer = pd.passing == ParamPassingKind::Pointer;
        pd.is_reference = pd.passing == ParamPassingKind::ConstRef ||
                          pd.passing == ParamPassingKind::MutableRef;
        auto param_dims = arrayDimsAfterName(cursorText(param), pd.name); // UNSAFE_TEXT_FALLBACK_ALLOW: C array parameter dimension recovery.
        if (!param_dims.empty()) {
            pd.type.is_array = true;
            pd.type.array_dims = param_dims;
            pd.type.array_size = param_dims.front();
            if (pd.passing == ParamPassingKind::Value) {
                pd.passing = ParamPassingKind::Pointer;
                pd.is_pointer = true;
                pd.type.is_pointer = true;
                pd.type.is_mutable = !pd.type.is_const;
            }
        }
        pd.is_output = false;
        func.params.push_back(pd);
    }

    auto func_children = getChildren(cursor);
    for (auto& child : func_children) {
        if (clang_getCursorKind(child) == CXCursor_CompoundStmt) {
            func.body = convertBlock(child);
            break;
        }
    }

    std::unordered_set<std::string> written;
    std::unordered_set<std::string> read;
    collectReadBases(func.body, read);
    collectWrittenBases(func.body, written);
    for (auto& p : func.params) {
        p.direction = inferParamDirection(p, read, written);
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
            ParamDecl p;
            p.name = cxToStr(clang_getCursorSpelling(c));
            p.type = convertType(clang_getCursorType(c));
            p.is_output = false;
            fn->params.push_back(p);
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
                ParamDecl p;
                p.name = cxToStr(clang_getCursorSpelling(mc));
                p.type = convertType(clang_getCursorType(mc));
                p.is_output = false;
                ctx->fn->params.push_back(p);
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
                ParamDecl p;
                p.name = cxToStr(clang_getCursorSpelling(c));
                p.type = convertType(clang_getCursorType(c));
                p.is_output = false;
                fn->params.push_back(p);
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

    if (allowUnsafeTextFallback && fn->params.empty() && fn->body.empty()) {
        std::string text = cursorText(lambda_cursor); // UNSAFE_TEXT_FALLBACK_ALLOW: disabled lambda text fallback.
        auto l = text.find('(');
        auto r = text.find(')', l == std::string::npos ? 0 : l);
        if (l != std::string::npos && r != std::string::npos && l < r) {
            std::string params = text.substr(l + 1, r - l - 1);
            std::stringstream ss(params);
            std::string one;
            while (std::getline(ss, one, ',')) {
                one.erase(0, one.find_first_not_of(" \t\r\n"));
                one.erase(one.find_last_not_of(" \t\r\n") + 1);
                auto sp = one.find_last_of(" \t");
                if (sp != std::string::npos) {
                    ParamDecl p;
                    p.type.name = one.substr(0, sp);
                    p.name = one.substr(sp + 1);
                    if (p.type.name.find("uint8") != std::string::npos) p.type.width = 8;
                    else if (p.type.name.find("uint16") != std::string::npos) p.type.width = 16;
                    else if (p.type.name.find("uint32") != std::string::npos) p.type.width = 32;
                    else if (p.type.name == "bool") p.type.width = 1;
                    p.is_output = false;
                    fn->params.push_back(p);
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
    stmt->decl_type.init_values = integerTokensInBraces(cursorText(c, true)); // UNSAFE_TEXT_FALLBACK_ALLOW: static lookup-table initializer extraction.
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

// --- Public API ---

BuildResult buildASTFromSource(const std::string& source_file,
                               const std::string& top_function,
                               const std::vector<std::string>& extra_args) {
    BuildResult result;
    lambda_operator_usr_to_name.clear();
    lambda_operator_location_to_name.clear();
    lambda_operator_signature_to_name.clear();

    CXIndex index = clang_createIndex(0, 0);

    std::vector<const char*> args;
    args.push_back("-std=c++17");
    args.push_back("-fsyntax-only");
    for (auto& a : extra_args) args.push_back(a.c_str());

    CXTranslationUnit tu = clang_parseTranslationUnit(
        index, source_file.c_str(),
        args.data(), static_cast<int>(args.size()),
        nullptr, 0,
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
    struct FindCtx {
        std::string target;
        CXCursor found;
        bool success = false;
    } ctx;
    ctx.target = top_function;
    ctx.found = clang_getNullCursor();

    clang_visitChildren(root, [](CXCursor c, CXCursor, CXClientData data) -> CXChildVisitResult {
        auto* ctx = static_cast<FindCtx*>(data);
        if (clang_getCursorKind(c) == CXCursor_FunctionDecl) {
            std::string name = cxToStr(clang_getCursorSpelling(c));
            if (name == ctx->target && clang_isCursorDefinition(c)) {
                ctx->found = c;
                ctx->success = true;
                return CXChildVisit_Break;
            }
        }
        return CXChildVisit_Continue;
    }, &ctx);

    if (!ctx.success) {
        result.error = "Function '" + top_function + "' not found in " + source_file;
        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(index);
        return result;
    }

    std::string top_source_file = cursorFileName(ctx.found);
    auto source_functions = collectSourceFunctionDefinitions(root, top_source_file);
    bool saw_top = false;
    for (const auto& fn : source_functions) {
        if (fn.name == top_function && clang_equalCursors(fn.cursor, ctx.found)) {
            saw_top = true;
            break;
        }
    }
    if (!saw_top) {
        source_functions.push_back({top_function, ctx.found});
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

    FunctionAST func = convertFunctionDecl(ctx.found, top_function);
    collectStructFieldLayouts(root, func);
    collectLocalLambdas(ctx.found, func);
    for (auto& [name, lambda] : func.lambdas) {
        if (!lambda) continue;
        auto unrolled = unrollLoops(lambda->body);
        if (!unrolled.error.empty()) {
            result.error = "Failed to unroll lambda '" + name + "': " + unrolled.error;
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return result;
        }
        lambda->body = std::move(unrolled.body);
    }

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
        if (fn.name == func.name && clang_equalCursors(fn.cursor, ctx.found)) continue;
        if (fn.name == func.name) continue;
        auto helper = std::make_shared<FunctionAST>(convertFunctionDecl(fn.cursor, fn.name));
        auto unrolled = unrollLoops(helper->body);
        if (!unrolled.error.empty()) {
            result.error = "Failed to unroll helper '" + fn.name + "': " + unrolled.error;
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(index);
            return result;
        }
        helper->body = std::move(unrolled.body);
        func.helpers.push_back(helper);
    }

    result.function = func;

    clang_disposeTranslationUnit(tu);
    clang_disposeIndex(index);
    return result;
}

} // namespace pred
