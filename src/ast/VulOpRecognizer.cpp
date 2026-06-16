#include "ast/VulOpRecognizer.h"

#include <algorithm>
#include <cctype>

namespace pred {

std::optional<int> recognizeTemplateInt(CXCursor cursor, int index) {
    int count = clang_Cursor_getNumTemplateArguments(cursor);
    if (count <= index) return std::nullopt;
    if (clang_Cursor_getTemplateArgumentKind(cursor, index) != CXTemplateArgumentKind_Integral) {
        return std::nullopt;
    }
    return static_cast<int>(clang_Cursor_getTemplateArgumentValue(cursor, index));
}

namespace {

std::string cxToStrLocal(CXString s) {
    const char* c = clang_getCString(s);
    std::string out = c ? c : "";
    clang_disposeString(s);
    return out;
}

std::vector<CXCursor> childrenOf(CXCursor cursor) {
    std::vector<CXCursor> children;
    clang_visitChildren(cursor, [](CXCursor c, CXCursor, CXClientData data) {
        auto* out = static_cast<std::vector<CXCursor>*>(data);
        out->push_back(c);
        return CXChildVisit_Continue;
    }, &children);
    return children;
}

std::string cursorSpelling(CXCursor cursor) {
    std::string spelling = cxToStrLocal(clang_getCursorSpelling(cursor));
    if (!spelling.empty()) return spelling;
    CXCursor referenced = clang_getCursorReferenced(cursor);
    if (!clang_equalCursors(referenced, clang_getNullCursor())) {
        return cxToStrLocal(clang_getCursorSpelling(referenced));
    }
    return "";
}

std::string canonicalCallName(std::string name) {
    if (name.empty()) return name;
    name.erase(std::remove_if(name.begin(), name.end(), [](unsigned char c) {
        return std::isspace(c);
    }), name.end());
    auto scope = name.rfind("::");
    if (scope != std::string::npos) name = name.substr(scope + 2);
    auto lt = name.find('<');
    if (lt != std::string::npos) name = name.substr(0, lt);
    return name;
}

std::string receiverNameFromCursor(CXCursor cursor) {
    std::string name = cursorSpelling(cursor);
    if (!name.empty()) return name;
    auto children = childrenOf(cursor);
    for (auto& child : children) {
        name = cursorSpelling(child);
        if (!name.empty()) return name;
    }
    return "";
}

bool isMemberRef(CXCursor cursor) {
    return clang_getCursorKind(cursor) == CXCursor_MemberRefExpr;
}

bool isExactName(const std::string& name, const char* expected) {
    return name == expected;
}

} // namespace

VulCallInfo recognizeVulCall(CXCursor cursor,
                             const std::vector<CXCursor>& children,
                             const std::string& spelling,
                             const std::string& first_child_spelling) {
    std::string name = canonicalCallName(spelling.empty() ? first_child_spelling : spelling);
    if (canonicalCallName(first_child_spelling).rfind("operator", 0) == 0) {
        name = canonicalCallName(first_child_spelling);
    }
    VulCallInfo out;
    out.source_cursor_kind = clang_getCursorKind(cursor);
    out.normal_arg_count = static_cast<int>(children.size());

    bool has_member_ref = !children.empty() && isMemberRef(children.front());
    if (has_member_ref) {
        std::string member_name = canonicalCallName(cursorSpelling(children.front()));
        if (!member_name.empty()) {
            name = member_name;
        }
    }
    out.method_name = name;

    if (has_member_ref) {
        auto member_children = childrenOf(children.front());
        if (!member_children.empty()) {
            out.has_receiver = true;
            out.receiver_cursor = member_children.front();
            out.receiver_expr_kind = clang_getCursorKind(member_children.front());
            out.receiver_name = receiverNameFromCursor(out.receiver_cursor);
            out.normal_arg_count = std::max(0, static_cast<int>(children.size()) - 1);
        }
    }
    size_t start = has_member_ref ? 1 : 0;
    if (name.rfind("operator", 0) == 0 && children.size() > 1 &&
        canonicalCallName(first_child_spelling).rfind("operator", 0) == 0) {
        start = 1;
    }
    for (size_t i = start; i < children.size(); ++i) {
        out.normal_arg_cursors.push_back(children[i]);
    }
    if (out.has_receiver && !out.normal_arg_cursors.empty()) {
        std::string first_arg_name = cursorSpelling(out.normal_arg_cursors.front());
        if (!out.receiver_name.empty() && first_arg_name == out.receiver_name) {
            out.normal_arg_cursors.erase(out.normal_arg_cursors.begin());
        }
    }
    out.normal_arg_count = static_cast<int>(out.normal_arg_cursors.size());

    if (isExactName(name, "setnext")) out.kind = VulCallKind::SetNext;
    else if (isExactName(name, "call")) out.kind = VulCallKind::ReqHelperCall;
    else if (isExactName(name, "output")) out.kind = VulCallKind::Output;
    else if (isExactName(name, "cat") || isExactName(name, "concat") || isExactName(name, "Cat")) out.kind = VulCallKind::Cat;
    else if (isExactName(name, "repeat")) out.kind = VulCallKind::Repeat;
    else if (isExactName(name, "reduce_or")) out.kind = VulCallKind::ReduceOr;
    else if (isExactName(name, "reduce_and")) out.kind = VulCallKind::ReduceAnd;
    else if (isExactName(name, "reduce_xor")) out.kind = VulCallKind::ReduceXor;
    else if (isExactName(name, "zext")) out.kind = VulCallKind::ZExt;
    else if (isExactName(name, "trunc")) out.kind = VulCallKind::Trunc;
    else if (isExactName(name, "get")) out.kind = VulCallKind::RegProxyGet;
    else if (isExactName(name, "sint")) out.kind = VulCallKind::SignedView;
    else if (isExactName(name, "operator()")) out.kind = VulCallKind::OperatorCall;
    else if (isExactName(name, "range_at")) out.kind = VulCallKind::RangeAt;
    else if (isExactName(name, "bit_at")) out.kind = VulCallKind::BitAt;
    out.template_value = recognizeTemplateInt(cursor, 0);
    return out;
}

} // namespace pred
