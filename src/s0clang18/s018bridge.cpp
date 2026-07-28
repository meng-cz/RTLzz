#include "s0clang18/s018bridge.hpp"

#include "s0clang18/s0clang18.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace pred::s0clang18 {
namespace {

using pred::s0ast::EntityId;
using pred::s0ast::S0Expr;
using pred::s0ast::S0ExprKind;
using pred::s0ast::S0ExprPtr;
using pred::s0ast::S0Function;
using pred::s0ast::S0FunctionKind;
using pred::s0ast::S0Param;
using pred::s0ast::S0Program;
using pred::s0ast::S0Stmt;
using pred::s0ast::S0StmtKind;
using pred::s0ast::S0StmtPtr;
using pred::s0ast::S0Type;
using pred::v2::ExprKind;
using pred::v2::ExprPtr;
using pred::v2::FunctionAST;
using pred::v2::ParamDecl;
using pred::v2::StmtKind;
using pred::v2::StmtPtr;
using pred::v2::StructConstructorInfo;
using pred::v2::StructFieldInfo;
using pred::v2::TypeInfo;

S0Type makeS0Type(const TypeInfo& type) {
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
    out.type = makeS0Type(param.type);
    out.debug_loc = param.debug_loc;
    out.direction = param.direction;
    out.passing = param.passing;
    return out;
}

S0ExprKind convertExprKind(ExprKind kind) {
    switch (kind) {
    case ExprKind::Literal:
        return S0ExprKind::Literal;
    case ExprKind::VarRef:
        return S0ExprKind::VarRef;
    case ExprKind::UnaryOp:
        return S0ExprKind::Unary;
    case ExprKind::BinaryOp:
        return S0ExprKind::Binary;
    case ExprKind::Ternary:
        return S0ExprKind::Ternary;
    case ExprKind::Call:
        return S0ExprKind::Call;
    case ExprKind::Cast:
        return S0ExprKind::Cast;
    case ExprKind::ArrayAccess:
        return S0ExprKind::ArrayAccess;
    case ExprKind::FieldAccess:
        return S0ExprKind::FieldAccess;
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
    case StmtKind::Decl:
        return S0StmtKind::Decl;
    case StmtKind::Assign:
        return S0StmtKind::Assign;
    case StmtKind::If:
        return S0StmtKind::If;
    case StmtKind::For:
        return S0StmtKind::For;
    case StmtKind::While:
        return S0StmtKind::While;
    case StmtKind::DoWhile:
        return S0StmtKind::DoWhile;
    case StmtKind::Switch:
        return S0StmtKind::Switch;
    case StmtKind::Block:
        return S0StmtKind::Block;
    case StmtKind::Break:
        return S0StmtKind::Break;
    case StmtKind::Continue:
        return S0StmtKind::Continue;
    case StmtKind::Return:
        return S0StmtKind::Return;
    case StmtKind::ExprStmt:
        return S0StmtKind::ExprStmt;
    }
    return S0StmtKind::ExprStmt;
}

struct ConvertContext {
    EntityId next_expr = 0;
    EntityId next_stmt = 0;
    EntityId next_param = 0;
};

S0ExprPtr convertExpr(const ExprPtr& expr, ConvertContext& context);
S0StmtPtr convertStmt(const StmtPtr& stmt, ConvertContext& context);

void addExpr(std::vector<S0ExprPtr>& out,
             const ExprPtr& expr,
             ConvertContext& context) {
    if (auto converted = convertExpr(expr, context)) {
        out.push_back(std::move(converted));
    }
}

S0ExprPtr convertExpr(const ExprPtr& expr, ConvertContext& context) {
    if (!expr) return nullptr;
    auto out = std::make_shared<S0Expr>();
    out->id = context.next_expr++;
    out->kind = convertExprKind(expr->kind);
    out->type = makeS0Type(expr->type);
    out->debug_loc = expr->debug_loc;
    out->text = expr->literal_value;
    out->name = expr->var_name.empty() ? expr->callee : expr->var_name;
    out->op = expr->op;
    if (expr->kind == ExprKind::FieldAccess) out->name = expr->field_name;

    addExpr(out->operands, expr->left, context);
    addExpr(out->operands, expr->right, context);
    addExpr(out->operands, expr->operand, context);
    addExpr(out->operands, expr->array_base, context);
    addExpr(out->operands, expr->index, context);
    addExpr(out->operands, expr->struct_base, context);
    addExpr(out->operands, expr->cast_expr, context);
    addExpr(out->operands, expr->cond, context);
    addExpr(out->operands, expr->then_expr, context);
    addExpr(out->operands, expr->else_expr, context);
    addExpr(out->operands, expr->base, context);
    addExpr(out->operands, expr->value, context);
    for (const auto& arg : expr->args) addExpr(out->operands, arg, context);
    for (const auto& part : expr->parts) addExpr(out->operands, part, context);

    if (expr->hi >= 0) out->template_args.push_back(expr->hi);
    if (expr->lo >= 0) out->template_args.push_back(expr->lo);
    if (expr->bit >= 0) out->template_args.push_back(expr->bit);
    if (expr->times > 0) out->template_args.push_back(expr->times);
    if (expr->to_width > 0) out->template_args.push_back(expr->to_width);
    return out;
}

void addStmtList(std::vector<S0StmtPtr>& out,
                 const std::vector<StmtPtr>& stmts,
                 ConvertContext& context) {
    for (const auto& stmt : stmts) {
        if (auto converted = convertStmt(stmt, context)) {
            out.push_back(std::move(converted));
        }
    }
}

S0StmtPtr convertStmt(const StmtPtr& stmt, ConvertContext& context) {
    if (!stmt) return nullptr;
    auto out = std::make_shared<S0Stmt>();
    out->id = context.next_stmt++;
    out->kind = convertStmtKind(stmt->kind);
    out->debug_loc = stmt->debug_loc;
    out->name = stmt->decl_name;
    out->type = makeS0Type(stmt->decl_type);

    addExpr(out->exprs, stmt->assign_target, context);
    addExpr(out->exprs, stmt->assign_value, context);
    if (stmt->decl_init) addExpr(out->exprs, *stmt->decl_init, context);
    for (const auto& arg : stmt->decl_init_args) addExpr(out->exprs, arg, context);
    addExpr(out->exprs, stmt->if_cond, context);
    addExpr(out->exprs, stmt->for_cond, context);
    addExpr(out->exprs, stmt->for_step, context);
    addExpr(out->exprs, stmt->while_cond, context);
    addExpr(out->exprs, stmt->switch_expr, context);
    if (stmt->return_value) addExpr(out->exprs, *stmt->return_value, context);
    addExpr(out->exprs, stmt->expr_stmt, context);

    if (stmt->for_init) {
        if (auto converted = convertStmt(stmt->for_init, context)) {
            out->children.push_back(std::move(converted));
        }
    }
    addStmtList(out->children, stmt->if_then, context);
    addStmtList(out->children, stmt->if_else, context);
    addStmtList(out->children, stmt->for_body, context);
    addStmtList(out->children, stmt->while_body, context);
    addStmtList(out->children, stmt->block_stmts, context);
    for (const auto& clause : stmt->switch_cases) {
        if (clause.value) addExpr(out->exprs, *clause.value, context);
        addStmtList(out->children, clause.body, context);
    }
    return out;
}

void addRecordAlias(FunctionAST& function,
                    const std::string& name,
                    const std::vector<StructFieldInfo>& fields,
                    const std::vector<StructConstructorInfo>& constructors) {
    if (name.empty()) return;
    function.struct_fields[name] = fields;
    if (!constructors.empty()) function.struct_constructors[name] = constructors;
}

void attachRecordMetadata(FunctionAST& function, const RecordMetadataSet* records) {
    if (!records) return;
    for (const RecordMetadata& record : records->records) {
        std::vector<StructFieldInfo> fields;
        fields.reserve(record.fields.size());
        for (const RecordField& field : record.fields) {
            StructFieldInfo out;
            out.name = field.name;
            out.type = field.type;
            fields.push_back(std::move(out));
        }

        std::vector<StructConstructorInfo> constructors;
        constructors.reserve(record.constructors.size());
        for (const RecordConstructor& constructor : record.constructors) {
            StructConstructorInfo out;
            out.param_names = constructor.param_names;
            out.field_to_param = constructor.field_to_param;
            constructors.push_back(std::move(out));
        }

        addRecordAlias(function, record.key.canonical_name, fields, constructors);
        addRecordAlias(function, "struct " + record.key.canonical_name,
                       fields, constructors);
    }
}

void appendFunction(S0Program& program,
                    const FunctionAST& function,
                    S0FunctionKind kind,
                    ConvertContext& context) {
    S0Function out;
    out.id = static_cast<EntityId>(program.functions.size());
    out.name = function.name;
    out.kind = kind;
    out.return_type = makeS0Type(function.return_type);
    if (!function.params.empty()) out.debug_loc = function.params.front().debug_loc;
    for (const ParamDecl& param : function.params) {
        out.params.push_back(convertParam(param, context.next_param++));
    }
    addStmtList(out.body, function.body, context);
    program.functions.push_back(std::move(out));
}

void appendNestedFunctions(S0Program& program,
                           const FunctionAST& function,
                           S0FunctionKind kind,
                           ConvertContext& context,
                           std::unordered_set<std::string>& seen) {
    for (const auto& helper : function.helpers) {
        if (!helper || helper->name.empty() || seen.count(helper->name)) continue;
        seen.insert(helper->name);
        appendFunction(program, *helper, S0FunctionKind::Helper, context);
        appendNestedFunctions(program, *helper, S0FunctionKind::Helper, context, seen);
    }
    for (const auto& [name, lambda] : function.lambdas) {
        if (!lambda) continue;
        const std::string stable_name = lambda->name.empty() ? name : lambda->name;
        if (stable_name.empty() || seen.count(stable_name)) continue;
        seen.insert(stable_name);
        appendFunction(program, *lambda, S0FunctionKind::Lambda, context);
        appendNestedFunctions(program, *lambda, S0FunctionKind::Lambda, context, seen);
    }
    (void)kind;
}

Diagnostic firstErrorOrDefault(const std::vector<Diagnostic>& diagnostics,
                               const std::string& message,
                               const std::string& stage) {
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity == Severity::Error) return diagnostic;
    }
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = message;
    diagnostic.context.stage = stage;
    return diagnostic;
}

void appendWarnings(Clang18BuildResult& result,
                    const std::vector<Diagnostic>& diagnostics) {
    for (const Diagnostic& diagnostic : diagnostics) {
        if (diagnostic.severity != Severity::Error) {
            result.warnings.push_back(diagnostic);
        }
    }
}

Clang18PipelineState makeBridgeState(const S0Clang18PipelineState& input,
                                     Clang18Options options) {
    Clang18PipelineState out;
    out.options = std::move(options);
    if (input.ports) out.ports = *input.ports;
    if (input.top) out.top = *input.top;
    if (input.semantic_index) out.semantic_index = *input.semantic_index;
    if (input.records) out.records = *input.records;
    if (input.reachability) out.reachability = *input.reachability;
    if (input.template_specializations) {
        out.template_specializations = *input.template_specializations;
    }
    if (input.lambdas) out.lambdas = *input.lambdas;
    if (input.surface_function) out.surface_ast = *input.surface_function;
    out.diagnostics = input.diagnostics;
    return out;
}

} // namespace

S0Program bridgeToS0Program(const Clang18PipelineState& state,
                            FunctionAST surface_ast) {
    attachRecordMetadata(surface_ast, state.records ? &*state.records : nullptr);

    S0Program program;
    program.source_name = !state.options.source_name.empty()
        ? state.options.source_name
        : (state.session ? state.session->main_file_path : std::string{});
    program.struct_fields = surface_ast.struct_fields;
    program.struct_constructors = surface_ast.struct_constructors;

    ConvertContext context;
    appendFunction(program, surface_ast, S0FunctionKind::Top, context);
    program.top_function = 0;

    std::unordered_set<std::string> seen;
    if (!surface_ast.name.empty()) seen.insert(surface_ast.name);
    appendNestedFunctions(program, surface_ast, S0FunctionKind::Helper, context, seen);

    program.surface_ast = std::move(surface_ast);
    return program;
}

std::string debugPrint(const Clang18PipelineState& state) {
    std::ostringstream os;
    os << "s0clang18 bridge\n";
    os << "source: " << state.options.source_name << "\n";
    os << "top_request: " << state.options.top_function << "\n";
    if (state.session) os << "main_file: " << state.session->main_file_path << "\n";
    if (state.ports) os << "ports: " << state.ports->ports.size() << "\n";
    if (state.top) os << "top: " << state.top->resolved_name << "\n";
    if (state.reachability) {
        os << "reachable_functions: " << state.reachability->functions.size() << "\n";
    }
    if (state.records) os << "records: " << state.records->records.size() << "\n";
    if (state.surface_ast) {
        os << "surface_ast: " << state.surface_ast->name
           << " params=" << state.surface_ast->params.size()
           << " stmts=" << state.surface_ast->body.size()
           << " helpers=" << state.surface_ast->helpers.size() << "\n";
    }
    if (!state.diagnostics.empty()) {
        os << "diagnostics: " << state.diagnostics.size() << "\n";
        for (const Diagnostic& diagnostic : state.diagnostics) {
            os << "  - " << diagnosticLabel(diagnostic) << "\n";
        }
    }
    return os.str();
}

Clang18BuildResult buildS0ProgramWithClang18(const Clang18Options& options) {
    Clang18BuildResult result;

    S0Clang18PipelineOptions pipeline_options;
    pipeline_options.clang = options;
    pipeline_options.stop_after = S0Clang18Step::GlobalPortLift;
    auto pipeline = runS0Clang18Pipeline(pipeline_options);
    appendWarnings(result, pipeline.diagnostics);

    if (!pipeline.ok() || !pipeline.value || !pipeline.value->surface_function ||
        !pipeline.value->ports || !pipeline.value->records ||
        !pipeline.value->reachability) {
        result.error = firstErrorOrDefault(
            pipeline.diagnostics,
            "S0Clang18 pipeline did not produce a complete surface AST",
            "s0clang18.18");
        if (pipeline.value) {
            Clang18PipelineState bridge_state =
                makeBridgeState(*pipeline.value, options);
            result.debug_text = debugPrint(bridge_state);
        }
        return result;
    }

    auto validation = validateSurfaceAST(*pipeline.value->surface_function,
                                         *pipeline.value->ports,
                                         *pipeline.value->records,
                                         *pipeline.value->reachability);
    appendWarnings(result, validation.diagnostics);
    if (!validation.ok()) {
        result.error = firstErrorOrDefault(
            validation.diagnostics,
            "S0Clang18 surface validation failed before bridge",
            "s0clang18.17");
        Clang18PipelineState bridge_state = makeBridgeState(*pipeline.value, options);
        bridge_state.diagnostics.insert(bridge_state.diagnostics.end(),
                                        validation.diagnostics.begin(),
                                        validation.diagnostics.end());
        result.debug_text = debugPrint(bridge_state);
        return result;
    }

    Clang18PipelineState bridge_state = makeBridgeState(*pipeline.value, options);
    result.program = bridgeToS0Program(
        bridge_state, std::move(*pipeline.value->surface_function));
    if (options.debug_print) {
        result.debug_text = debugPrint(bridge_state);
        if (result.program) {
            result.debug_text += pred::s0ast::debugPrint(*result.program);
        }
    }
    return result;
}

} // namespace pred::s0clang18
