#pragma once

#include <clang-c/Index.h>
#include <optional>
#include <string>
#include <vector>

namespace pred {

enum class VulCallKind {
    None,
    SetNext,
    ReqHelperCall,
    Output,
    Cat,
    Repeat,
    ReduceOr,
    ReduceAnd,
    ReduceXor,
    ZExt,
    Trunc,
    RegProxyGet,
    SignedView,
    OperatorCall,
    At,
    Pick,
    RangeAt,
    BitAt,
};

struct VulCallInfo {
    VulCallKind kind = VulCallKind::None;
    std::string method_name;
    std::string receiver_name;
    bool has_receiver = false;
    CXCursor receiver_cursor{};
    CXCursorKind receiver_expr_kind = CXCursor_UnexposedExpr;
    int normal_arg_count = 0;
    std::vector<CXCursor> normal_arg_cursors;
    CXCursorKind source_cursor_kind = CXCursor_UnexposedExpr;
    std::optional<int> template_value;
    std::vector<int> template_values;
};

std::optional<int> recognizeTemplateInt(CXCursor cursor, int index = 0);
VulCallInfo recognizeVulCall(CXCursor cursor,
                             const std::vector<CXCursor>& children,
                             const std::string& spelling,
                             const std::string& first_child_spelling);

} // namespace pred
