#include "s0ast/S0AST.h"

#include "s0ast/S0NativeASTBuilder.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>
#include <utility>

namespace pred::s0ast {

using TypeInfo = pred::v2::TypeInfo;
using ParamDecl = pred::v2::ParamDecl;
using ExprPtr = pred::v2::ExprPtr;
using ExprKind = pred::v2::ExprKind;
using StmtPtr = pred::v2::StmtPtr;
using StmtKind = pred::v2::StmtKind;
using FunctionAST = pred::v2::FunctionAST;

namespace {

ErrorContext makeContext(S0Substage substage,
                         DebugLoc loc = {},
                         std::string note = {}) {
    ErrorContext context;
    context.stage = substageName(substage);
    context.loc = std::move(loc);
    context.source_file = context.loc.file;
    context.note = std::move(note);
    return context;
}

std::string formatException(const RTLZZException& ex) {
    return formatRTLZZExceptionMessage(ex.message(), ex.contextStack());
}

S0Diagnostic makeDiagnostic(S0Substage substage, std::string message) {
    S0Diagnostic diagnostic;
    diagnostic.context = makeContext(substage);
    diagnostic.message = std::move(message);
    return diagnostic;
}

S0Type makeType(const TypeInfo& type) {
    S0Type out;
    out.signed_view = type.hw_kind == "signed_view" ||
                      type.name.rfind("IntSignedView<", 0) == 0;
    out.type = type;
    return out;
}

S0Param convertParam(const ParamDecl& param, EntityId id) {
    S0Param out;
    out.id = id;
    out.name = param.name;
    out.type = makeType(param.type);
    out.debug_loc = param.debug_loc;
    out.direction = param.direction;
    out.passing = param.passing;
    return out;
}

S0ExprKind convertExprKind(ExprKind kind) {
    switch (kind) {
    case ExprKind::Literal: return S0ExprKind::Literal;
    case ExprKind::VarRef: return S0ExprKind::VarRef;
    case ExprKind::UnaryOp: return S0ExprKind::Unary;
    case ExprKind::BinaryOp: return S0ExprKind::Binary;
    case ExprKind::Ternary: return S0ExprKind::Ternary;
    case ExprKind::Call: return S0ExprKind::Call;
    case ExprKind::Cast: return S0ExprKind::Cast;
    case ExprKind::ArrayAccess: return S0ExprKind::ArrayAccess;
    case ExprKind::FieldAccess: return S0ExprKind::FieldAccess;
    case ExprKind::ZExt:
    case ExprKind::SExt:
    case ExprKind::Trunc:
    case ExprKind::Slice:
    case ExprKind::BitSelect:
    case ExprKind::WriteSlice:
    case ExprKind::WriteBit:
    case ExprKind::DynamicWriteSlice:
    case ExprKind::DynamicWriteBit:
    case ExprKind::Concat:
    case ExprKind::Repeat:
    case ExprKind::ReduceOr:
    case ExprKind::ReduceAnd:
    case ExprKind::ReduceXor:
        return S0ExprKind::HardwareSurface;
    }
    return S0ExprKind::Literal;
}

S0StmtKind convertStmtKind(StmtKind kind) {
    switch (kind) {
    case StmtKind::Decl: return S0StmtKind::Decl;
    case StmtKind::Assign: return S0StmtKind::Assign;
    case StmtKind::If: return S0StmtKind::If;
    case StmtKind::For: return S0StmtKind::For;
    case StmtKind::While: return S0StmtKind::While;
    case StmtKind::DoWhile: return S0StmtKind::DoWhile;
    case StmtKind::Switch: return S0StmtKind::Switch;
    case StmtKind::Block: return S0StmtKind::Block;
    case StmtKind::Break: return S0StmtKind::Break;
    case StmtKind::Continue: return S0StmtKind::Continue;
    case StmtKind::Return: return S0StmtKind::Return;
    case StmtKind::ExprStmt: return S0StmtKind::ExprStmt;
    }
    return S0StmtKind::ExprStmt;
}

struct ConvertCtx {
    EntityId next_expr = 0;
    EntityId next_stmt = 0;
    EntityId next_param = 0;
};

S0ExprPtr convertExpr(const ExprPtr& expr, ConvertCtx& ctx);

void addExpr(std::vector<S0ExprPtr>& out, const ExprPtr& expr, ConvertCtx& ctx) {
    if (auto converted = convertExpr(expr, ctx)) out.push_back(std::move(converted));
}

S0ExprPtr convertExpr(const ExprPtr& expr, ConvertCtx& ctx) {
    if (!expr) return nullptr;
    auto out = std::make_shared<S0Expr>();
    out->id = ctx.next_expr++;
    out->kind = convertExprKind(expr->kind);
    out->type = makeType(expr->type);
    out->debug_loc = expr->debug_loc;
    out->text = expr->literal_value;
    out->name = expr->var_name.empty() ? expr->callee : expr->var_name;
    out->op = expr->op;
    if (expr->kind == ExprKind::FieldAccess) out->name = expr->field_name;
    addExpr(out->operands, expr->left, ctx);
    addExpr(out->operands, expr->right, ctx);
    addExpr(out->operands, expr->operand, ctx);
    addExpr(out->operands, expr->array_base, ctx);
    addExpr(out->operands, expr->index, ctx);
    addExpr(out->operands, expr->struct_base, ctx);
    addExpr(out->operands, expr->cast_expr, ctx);
    addExpr(out->operands, expr->cond, ctx);
    addExpr(out->operands, expr->then_expr, ctx);
    addExpr(out->operands, expr->else_expr, ctx);
    addExpr(out->operands, expr->base, ctx);
    addExpr(out->operands, expr->value, ctx);
    for (const auto& arg : expr->args) addExpr(out->operands, arg, ctx);
    for (const auto& part : expr->parts) addExpr(out->operands, part, ctx);
    if (expr->hi >= 0) out->template_args.push_back(expr->hi);
    if (expr->lo >= 0) out->template_args.push_back(expr->lo);
    if (expr->bit >= 0) out->template_args.push_back(expr->bit);
    if (expr->times > 0) out->template_args.push_back(expr->times);
    if (expr->to_width > 0) out->template_args.push_back(expr->to_width);
    return out;
}

S0StmtPtr convertStmt(const StmtPtr& stmt, ConvertCtx& ctx);

void addStmtList(std::vector<S0StmtPtr>& out,
                 const std::vector<StmtPtr>& stmts,
                 ConvertCtx& ctx) {
    for (const auto& stmt : stmts) {
        if (auto converted = convertStmt(stmt, ctx)) out.push_back(std::move(converted));
    }
}

S0StmtPtr convertStmt(const StmtPtr& stmt, ConvertCtx& ctx) {
    if (!stmt) return nullptr;
    auto out = std::make_shared<S0Stmt>();
    out->id = ctx.next_stmt++;
    out->kind = convertStmtKind(stmt->kind);
    out->debug_loc = stmt->debug_loc;
    out->name = stmt->decl_name;
    out->type = makeType(stmt->decl_type);
    addExpr(out->exprs, stmt->assign_target, ctx);
    addExpr(out->exprs, stmt->assign_value, ctx);
    if (stmt->decl_init) addExpr(out->exprs, *stmt->decl_init, ctx);
    for (const auto& arg : stmt->decl_init_args) addExpr(out->exprs, arg, ctx);
    addExpr(out->exprs, stmt->if_cond, ctx);
    addExpr(out->exprs, stmt->for_cond, ctx);
    addExpr(out->exprs, stmt->for_step, ctx);
    addExpr(out->exprs, stmt->while_cond, ctx);
    addExpr(out->exprs, stmt->switch_expr, ctx);
    if (stmt->return_value) addExpr(out->exprs, *stmt->return_value, ctx);
    addExpr(out->exprs, stmt->expr_stmt, ctx);
    if (stmt->for_init) out->children.push_back(convertStmt(stmt->for_init, ctx));
    addStmtList(out->children, stmt->if_then, ctx);
    addStmtList(out->children, stmt->if_else, ctx);
    addStmtList(out->children, stmt->for_body, ctx);
    addStmtList(out->children, stmt->while_body, ctx);
    addStmtList(out->children, stmt->block_stmts, ctx);
    for (const auto& clause : stmt->switch_cases) {
        if (clause.value) addExpr(out->exprs, *clause.value, ctx);
        addStmtList(out->children, clause.body, ctx);
    }
    return out;
}

bool containsUnsupportedPlaceholderExpr(const ExprPtr& expr, std::string& reason) {
    if (!expr) return false;
    if (expr->kind == ExprKind::Call &&
        expr->callee.rfind("__unsupported_", 0) == 0) {
        reason = "unsupported expression placeholder reached S0AST: " + expr->callee;
        return true;
    }
    for (const auto& child : {expr->left, expr->right, expr->operand, expr->array_base,
                              expr->index, expr->struct_base, expr->cast_expr, expr->cond,
                              expr->then_expr, expr->else_expr, expr->base, expr->value}) {
        if (containsUnsupportedPlaceholderExpr(child, reason)) return true;
    }
    for (const auto& arg : expr->args) {
        if (containsUnsupportedPlaceholderExpr(arg, reason)) return true;
    }
    for (const auto& part : expr->parts) {
        if (containsUnsupportedPlaceholderExpr(part, reason)) return true;
    }
    return false;
}

bool containsUnsupportedPlaceholderStmt(const StmtPtr& stmt, std::string& reason) {
    if (!stmt) return false;
    for (const auto& expr : {stmt->assign_target, stmt->assign_value, stmt->if_cond,
                             stmt->for_cond, stmt->for_step, stmt->while_cond,
                             stmt->switch_expr, stmt->expr_stmt}) {
        if (containsUnsupportedPlaceholderExpr(expr, reason)) return true;
    }
    if (stmt->decl_init && containsUnsupportedPlaceholderExpr(*stmt->decl_init, reason)) {
        return true;
    }
    if (stmt->return_value &&
        containsUnsupportedPlaceholderExpr(*stmt->return_value, reason)) {
        return true;
    }
    for (const auto& arg : stmt->decl_init_args) {
        if (containsUnsupportedPlaceholderExpr(arg, reason)) return true;
    }
    if (stmt->for_init && containsUnsupportedPlaceholderStmt(stmt->for_init, reason)) {
        return true;
    }
    for (const auto& child : stmt->if_then) {
        if (containsUnsupportedPlaceholderStmt(child, reason)) return true;
    }
    for (const auto& child : stmt->if_else) {
        if (containsUnsupportedPlaceholderStmt(child, reason)) return true;
    }
    for (const auto& child : stmt->for_body) {
        if (containsUnsupportedPlaceholderStmt(child, reason)) return true;
    }
    for (const auto& child : stmt->while_body) {
        if (containsUnsupportedPlaceholderStmt(child, reason)) return true;
    }
    for (const auto& child : stmt->block_stmts) {
        if (containsUnsupportedPlaceholderStmt(child, reason)) return true;
    }
    for (const auto& clause : stmt->switch_cases) {
        if (clause.value && containsUnsupportedPlaceholderExpr(*clause.value, reason)) {
            return true;
        }
        for (const auto& child : clause.body) {
            if (containsUnsupportedPlaceholderStmt(child, reason)) return true;
        }
    }
    return false;
}

bool containsUnsupportedPlaceholderFunction(const FunctionAST& fn, std::string& reason) {
    for (const auto& stmt : fn.body) {
        if (containsUnsupportedPlaceholderStmt(stmt, reason)) return true;
    }
    for (const auto& helper : fn.helpers) {
        if (helper && containsUnsupportedPlaceholderFunction(*helper, reason)) return true;
    }
    for (const auto& [_, lambda] : fn.lambdas) {
        if (lambda && containsUnsupportedPlaceholderFunction(*lambda, reason)) return true;
    }
    return false;
}

void appendFunction(S0Program& program,
                    const FunctionAST& fn,
                    S0FunctionKind kind,
                    ConvertCtx& ctx) {
    S0Function out;
    out.id = static_cast<EntityId>(program.functions.size());
    out.name = fn.name;
    out.kind = kind;
    out.return_type = makeType(fn.return_type);
    for (const auto& param : fn.params) {
        out.params.push_back(convertParam(param, ctx.next_param++));
    }
    addStmtList(out.body, fn.body, ctx);
    program.functions.push_back(std::move(out));
}

S0Program buildProgramFromNative(FunctionAST fn, const std::string& source_name) {
    S0Program program;
    program.source_name = source_name;
    program.struct_fields = fn.struct_fields;
    program.struct_constructors = fn.struct_constructors;
    ConvertCtx ctx;
    appendFunction(program, fn, S0FunctionKind::Top, ctx);
    program.top_function = 0;
    for (const auto& helper : fn.helpers) {
        if (helper) appendFunction(program, *helper, S0FunctionKind::Helper, ctx);
    }
    for (const auto& [_, lambda] : fn.lambdas) {
        if (lambda) appendFunction(program, *lambda, S0FunctionKind::Lambda, ctx);
    }
    program.surface_ast = std::move(fn);
    return program;
}

std::string readFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string trim(std::string text) {
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool identChar(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

std::vector<std::string> splitCommaList(const std::string& text) {
    std::vector<std::string> out;
    int depth = 0;
    std::size_t begin = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        char ch = i < text.size() ? text[i] : ',';
        if (ch == '<' || ch == '(' || ch == '[' || ch == '{') ++depth;
        else if (ch == '>' || ch == ')' || ch == ']' || ch == '}') --depth;
        else if (ch == ',' && depth == 0) {
            auto part = trim(text.substr(begin, i - begin));
            if (!part.empty()) out.push_back(std::move(part));
            begin = i + 1;
        }
    }
    return out;
}

std::string normalizeCaptureName(std::string capture) {
    capture = trim(std::move(capture));
    if (capture == "=" || capture == "&") return "";
    while (!capture.empty() && (capture.front() == '&' || capture.front() == '=')) {
        capture.erase(capture.begin());
        capture = trim(std::move(capture));
    }
    return capture;
}

std::size_t skipSpaces(const std::string& text, std::size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    return pos;
}

std::size_t findMatching(const std::string& text,
                         std::size_t open_pos,
                         char open_ch,
                         char close_ch) {
    int depth = 0;
    for (std::size_t i = open_pos; i < text.size(); ++i) {
        if (text[i] == open_ch) ++depth;
        else if (text[i] == close_ch) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

std::string findIdentifierType(const std::string& prefix, const std::string& name) {
    std::regex pattern("(?:^|[\\(,;\\{\\n])\\s*([A-Za-z_][A-Za-z0-9_:<> ]*(?:[&*]\\s*)?)\\s+" + name + "\\b");
    std::string found;
    for (std::sregex_iterator it(prefix.begin(), prefix.end(), pattern), end; it != end; ++it) {
        std::string candidate = trim((*it)[1].str());
        if (candidate.empty()) continue;
        if (candidate.find_first_of("=;(),{}[]") != std::string::npos) continue;
        if (candidate == "return" || candidate == "if" || candidate == "for" ||
            candidate == "while" || candidate == "auto") {
            continue;
        }
        found = std::move(candidate);
    }
    return found;
}

struct LambdaRewrite {
    std::string source_name;
    std::string generated_name;
    std::string return_type;
    std::string params;
    std::string body;
    std::vector<std::string> captures;
    std::vector<std::string> capture_types;
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::optional<LambdaRewrite> parseLambdaRewrite(const std::string& source,
                                                std::size_t auto_pos,
                                                int index,
                                                std::string& error) {
    std::size_t pos = auto_pos + 4;
    if (auto_pos > 0 && identChar(source[auto_pos - 1])) return std::nullopt;
    pos = skipSpaces(source, pos);
    if (pos >= source.size() ||
        !(std::isalpha(static_cast<unsigned char>(source[pos])) || source[pos] == '_')) {
        return std::nullopt;
    }
    std::size_t name_begin = pos;
    while (pos < source.size() && identChar(source[pos])) ++pos;
    std::string name = source.substr(name_begin, pos - name_begin);
    pos = skipSpaces(source, pos);
    if (pos >= source.size() || source[pos] != '=') return std::nullopt;
    pos = skipSpaces(source, pos + 1);
    if (pos >= source.size() || source[pos] != '[') return std::nullopt;
    std::size_t capture_end = findMatching(source, pos, '[', ']');
    if (capture_end == std::string::npos) return std::nullopt;
    std::string capture_text = source.substr(pos + 1, capture_end - pos - 1);
    pos = skipSpaces(source, capture_end + 1);
    if (pos >= source.size() || source[pos] != '(') return std::nullopt;
    std::size_t params_end = findMatching(source, pos, '(', ')');
    if (params_end == std::string::npos) return std::nullopt;
    std::string params = trim(source.substr(pos + 1, params_end - pos - 1));
    pos = skipSpaces(source, params_end + 1);
    if (source.compare(pos, 2, "->") != 0) return std::nullopt;
    pos = skipSpaces(source, pos + 2);
    std::size_t ret_begin = pos;
    std::size_t body_begin = source.find('{', pos);
    if (body_begin == std::string::npos) return std::nullopt;
    std::string return_type = trim(source.substr(ret_begin, body_begin - ret_begin));
    std::size_t body_end = findMatching(source, body_begin, '{', '}');
    if (body_end == std::string::npos) return std::nullopt;
    pos = skipSpaces(source, body_end + 1);
    if (pos >= source.size() || source[pos] != ';') return std::nullopt;

    LambdaRewrite rewrite;
    rewrite.source_name = name;
    rewrite.generated_name = "__s0_lambda_" + name + "_" + std::to_string(index);
    rewrite.return_type = std::move(return_type);
    rewrite.params = std::move(params);
    rewrite.body = source.substr(body_begin, body_end - body_begin + 1);
    rewrite.begin = auto_pos;
    rewrite.end = pos + 1;
    for (auto capture : splitCommaList(capture_text)) {
        capture = normalizeCaptureName(std::move(capture));
        if (capture.empty()) {
            error = "S0 lambda rewrite requires explicit captures; capture-default is not supported yet";
            return std::nullopt;
        }
        std::string type = findIdentifierType(source.substr(0, auto_pos), capture);
        if (type.empty()) {
            error = "S0 lambda rewrite could not infer capture type for '" + capture + "'";
            return std::nullopt;
        }
        rewrite.captures.push_back(std::move(capture));
        rewrite.capture_types.push_back(std::move(type));
    }
    return rewrite;
}

std::string blankPreserveLines(std::string text) {
    for (char& ch : text) {
        if (ch != '\n' && ch != '\r') ch = ' ';
    }
    return text;
}

std::string combinedParamList(const LambdaRewrite& rewrite) {
    std::vector<std::string> parts;
    for (std::size_t i = 0; i < rewrite.captures.size(); ++i) {
        parts.push_back(rewrite.capture_types[i] + " " + rewrite.captures[i]);
    }
    if (!rewrite.params.empty()) parts.push_back(rewrite.params);
    std::ostringstream os;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) os << ", ";
        os << parts[i];
    }
    return os.str();
}

std::string captureArgPrefix(const LambdaRewrite& rewrite) {
    if (rewrite.captures.empty()) return "";
    std::ostringstream os;
    for (const auto& capture : rewrite.captures) os << capture << ", ";
    return os.str();
}

void replaceLambdaCalls(std::string& source, const LambdaRewrite& rewrite) {
    std::string prefix = captureArgPrefix(rewrite);
    std::size_t pos = 0;
    while (pos < source.size()) {
        pos = source.find(rewrite.source_name, pos);
        if (pos == std::string::npos) break;
        if (pos > 0 && identChar(source[pos - 1])) {
            pos += rewrite.source_name.size();
            continue;
        }
        std::size_t after = pos + rewrite.source_name.size();
        if (after < source.size() && identChar(source[after])) {
            pos = after;
            continue;
        }
        std::size_t call_pos = skipSpaces(source, after);
        if (call_pos >= source.size() || source[call_pos] != '(') {
            pos = after;
            continue;
        }
        source.replace(pos, call_pos - pos + 1,
                       rewrite.generated_name + "(" + prefix);
        pos += rewrite.generated_name.size() + prefix.size() + 1;
    }
}

std::optional<std::string> rewriteLocalLambdasForNativeParser(const std::string& source,
                                                        std::string& error) {
    std::vector<LambdaRewrite> rewrites;
    std::size_t pos = 0;
    int index = 0;
    while ((pos = source.find("auto", pos)) != std::string::npos) {
        if (auto rewrite = parseLambdaRewrite(source, pos, index, error)) {
            rewrites.push_back(std::move(*rewrite));
            ++index;
            pos = rewrites.back().end;
            continue;
        }
        if (!error.empty()) return std::nullopt;
        pos += 4;
    }
    if (rewrites.empty()) return source;

    std::string transformed = source;
    for (auto it = rewrites.rbegin(); it != rewrites.rend(); ++it) {
        transformed.replace(it->begin, it->end - it->begin,
                            blankPreserveLines(source.substr(it->begin,
                                                             it->end - it->begin)));
    }
    for (const auto& rewrite : rewrites) replaceLambdaCalls(transformed, rewrite);

    std::ostringstream prototypes;
    std::ostringstream definitions;
    for (const auto& rewrite : rewrites) {
        std::string params = combinedParamList(rewrite);
        prototypes << rewrite.return_type << " " << rewrite.generated_name
                   << "(" << params << ");\n";
        definitions << "\n" << rewrite.return_type << " " << rewrite.generated_name
                    << "(" << params << ") " << rewrite.body << "\n";
    }

    std::size_t insert_pos = 0;
    std::size_t search = 0;
    while (search < transformed.size()) {
        std::size_t line_end = transformed.find('\n', search);
        std::size_t end = line_end == std::string::npos ? transformed.size() : line_end + 1;
        std::string line = transformed.substr(search, end - search);
        std::string stripped = trim(line);
        if (stripped.rfind("#include", 0) == 0) insert_pos = end;
        search = end;
        if (line_end == std::string::npos) break;
    }
    transformed.insert(insert_pos, prototypes.str());
    transformed += definitions.str();
    return transformed;
}

} // namespace

const char* substageName(S0Substage substage) {
    switch (substage) {
    case S0Substage::ClangSessionAndRawCursor: return "s0ast.0";
    case S0Substage::V2ASTDataModel: return "s0ast.1";
    case S0Substage::TypeAndEntityCollect: return "s0ast.2";
    case S0Substage::ExprStmtBuild: return "s0ast.3";
    case S0Substage::ResolveAndSurfaceValidate: return "s0ast.4";
    case S0Substage::PipelineBridge: return "s0ast.5";
    }
    return "s0ast";
}

S0Result parseProgram(const std::string& source_name,
                      const std::optional<std::string>& source_text,
                      const std::string& top_function,
                      const std::vector<std::string>& clang_args,
                      const S0ParseOptions& options) {
    S0Result result;
    try {
        ErrorContextGuard session_guard(makeContext(
            S0Substage::ClangSessionAndRawCursor, {}, "parse clang translation unit"));

        std::optional<std::string> native_source_text = source_text;
        if (!native_source_text) {
            native_source_text = readFile(source_name);
        }
        if (native_source_text) {
            std::string rewrite_error;
            auto rewritten = rewriteLocalLambdasForNativeParser(*native_source_text, rewrite_error);
            if (!rewritten) {
                result.error = makeDiagnostic(S0Substage::ExprStmtBuild, rewrite_error);
                return result;
            }
            native_source_text = std::move(*rewritten);
        }

        NativeBuildResult parsed;
        if (native_source_text) {
            parsed = buildV2ASTFromSourceText(source_name, *native_source_text,
                                              top_function, clang_args);
        } else {
            parsed = buildV2ASTFromSource(source_name, top_function, clang_args);
        }
        if (!parsed.error.empty()) {
            result.error = makeDiagnostic(S0Substage::ClangSessionAndRawCursor,
                                          parsed.error);
            return result;
        }
        if (!parsed.function) {
            result.error = makeDiagnostic(S0Substage::ClangSessionAndRawCursor,
                                          "failed to extract top function");
            return result;
        }

        std::string unsupported;
        {
            ErrorContextGuard build_guard(makeContext(
                S0Substage::ExprStmtBuild, {}, "build V2 surface AST"));
            if (containsUnsupportedPlaceholderFunction(*parsed.function, unsupported)) {
                result.error = makeDiagnostic(S0Substage::ExprStmtBuild, unsupported);
                return result;
            }
        }

        {
            ErrorContextGuard model_guard(makeContext(
                S0Substage::V2ASTDataModel, {}, "convert extracted AST to S0 data model"));
            result.program = buildProgramFromNative(std::move(*parsed.function), source_name);
        }
        if (result.program && options.debug_print) {
            result.debug_text = debugPrint(*result.program);
        }
        return result;
    } catch (const RTLZZException& ex) {
        result.error = makeDiagnostic(S0Substage::ClangSessionAndRawCursor,
                                      formatException(ex));
        return result;
    } catch (const std::exception& ex) {
        result.error = makeDiagnostic(S0Substage::ClangSessionAndRawCursor,
                                      ex.what());
        return result;
    }
}

const pred::v2::FunctionAST& surfaceAST(const S0Program& program) {
    return program.surface_ast;
}

std::string debugPrint(const S0Program& program) {
    std::ostringstream os;
    os << "s0ast\n";
    os << "source " << program.source_name << "\n";
    os << "top_function %" << program.top_function << "\n";
    os << "functions\n";
    for (const auto& fn : program.functions) {
        os << "  %" << fn.id << " ";
        switch (fn.kind) {
        case S0FunctionKind::Top: os << "top "; break;
        case S0FunctionKind::Helper: os << "helper "; break;
        case S0FunctionKind::Lambda: os << "lambda "; break;
        }
        os << fn.name << " params=" << fn.params.size()
           << " stmts=" << fn.body.size() << "\n";
    }
    os << "structs " << program.struct_fields.size() << "\n";
    return os.str();
}

} // namespace pred::s0ast
