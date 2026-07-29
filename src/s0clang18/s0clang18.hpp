#pragma once

#include "s0clang18/s016portlift.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pred::s0clang18 {

enum class S0Clang18Step {
    Session = 1,
    SourceAndDebugLoc = 2,
    PragmaAndPortDeclCollect = 3,
    TopFunctionSelect = 4,
    SemanticIndexBuild = 5,
    TypeLowering = 6,
    ConstEval = 7,
    RecordMetadataCollect = 8,
    FunctionReachabilityCollect = 9,
    TemplateSpecializationResolve = 10,
    LambdaCaptureResolve = 11,
    ExprBuild = 12,
    StmtBuild = 13,
    InitAndConstructBuild = 14,
    CallAndAPIBinding = 15,
    GlobalPortLift = 16,
};

struct S0Clang18PipelineOptions {
    Clang18Options clang;
    SourceLocPolicy loc_policy;
    TypeLoweringOptions type_options;
    S0Clang18Step stop_after = S0Clang18Step::GlobalPortLift;
};

struct S0Clang18PipelineState {
    S0Clang18PipelineOptions options;
    int completed_step = 0;
    std::optional<Clang18Session> session;
    std::optional<PortDeclTable> ports;
    std::optional<TopFunctionSelection> top;
    std::optional<SemanticIndex> semantic_index;
    std::optional<RecordMetadataSet> records;
    std::optional<FunctionReachabilityGraph> reachability;
    std::optional<TemplateSpecializationTable> template_specializations;
    std::optional<LambdaCaptureTable> lambdas;
    std::optional<pred::v2::FunctionAST> surface_function;
    std::optional<PortLiftPlan> port_lift_plan;
    std::vector<Diagnostic> diagnostics;
    std::string intermediate_text;
};

inline int stepNumber(S0Clang18Step step) {
    return static_cast<int>(step);
}

inline const char* stepName(S0Clang18Step step) {
    switch (step) {
    case S0Clang18Step::Session:
        return "s001session";
    case S0Clang18Step::SourceAndDebugLoc:
        return "s002sourceloc";
    case S0Clang18Step::PragmaAndPortDeclCollect:
        return "s003ports";
    case S0Clang18Step::TopFunctionSelect:
        return "s004top";
    case S0Clang18Step::SemanticIndexBuild:
        return "s005semantic";
    case S0Clang18Step::TypeLowering:
        return "s006type";
    case S0Clang18Step::ConstEval:
        return "s007consteval";
    case S0Clang18Step::RecordMetadataCollect:
        return "s008record";
    case S0Clang18Step::FunctionReachabilityCollect:
        return "s009reachability";
    case S0Clang18Step::TemplateSpecializationResolve:
        return "s010templates";
    case S0Clang18Step::LambdaCaptureResolve:
        return "s011lambdas";
    case S0Clang18Step::ExprBuild:
        return "s012expr";
    case S0Clang18Step::StmtBuild:
        return "s013stmt";
    case S0Clang18Step::InitAndConstructBuild:
        return "s014init";
    case S0Clang18Step::CallAndAPIBinding:
        return "s015calls";
    case S0Clang18Step::GlobalPortLift:
        return "s016portlift";
    }
    return "unknown";
}

inline bool hasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

inline bool isLambdaObjectType(const pred::v2::TypeInfo& type) {
    return type.name.find("(lambda") != std::string::npos ||
           type.struct_name.find("(lambda") != std::string::npos;
}

inline void appendDiagnostics(std::vector<Diagnostic>& out,
                              const std::vector<Diagnostic>& in) {
    out.insert(out.end(), in.begin(), in.end());
}

inline Diagnostic makePipelineDiagnostic(std::string stage,
                                         DebugLoc loc,
                                         std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = std::move(stage);
    diagnostic.context.loc = std::move(loc);
    diagnostic.context.source_file = diagnostic.context.loc.file;
    return diagnostic;
}

inline TypeLoweringContext makeTypeLoweringContext(
    const S0Clang18PipelineState& state) {
    TypeLoweringContext context;
    if (state.session) context.session = &*state.session;
    if (state.semantic_index) context.semantic_index = &*state.semantic_index;
    context.options = state.options.type_options;
    return context;
}

inline ConstEvalContext makeConstEvalContext(
    const S0Clang18PipelineState& state,
    const TypeLoweringContext& type_context) {
    ConstEvalContext context;
    if (state.session) context.session = &*state.session;
    context.type_context = &type_context;
    return context;
}

inline ExprBuildContext makeExprBuildContext(
    const S0Clang18PipelineState& state,
    const TypeLoweringContext& type_context,
    const ConstEvalContext& const_eval) {
    ExprBuildContext context;
    if (state.session) context.session = &*state.session;
    if (state.semantic_index) context.semantic_index = &*state.semantic_index;
    context.type_context = &type_context;
    context.const_eval = &const_eval;
    if (state.records) context.records = &*state.records;
    if (state.reachability) context.reachability = &*state.reachability;
    if (state.template_specializations) {
        context.templates = &*state.template_specializations;
    }
    if (state.lambdas) context.lambdas = &*state.lambdas;
    context.loc_policy = state.options.loc_policy;
    return context;
}

inline StmtBuildContext makeStmtBuildContext(
    const S0Clang18PipelineState& state,
    SemanticEntityId function_id,
    const TypeLoweringContext& type_context,
    const ConstEvalContext& const_eval) {
    StmtBuildContext context;
    context.current_function = function_id;
    context.expr_context = makeExprBuildContext(state, type_context, const_eval);
    return context;
}

inline std::string functionKindLabel(FunctionEntityKind kind) {
    switch (kind) {
    case FunctionEntityKind::Top:
        return "top";
    case FunctionEntityKind::Helper:
        return "helper";
    case FunctionEntityKind::FunctionTemplateSpecialization:
        return "function_template_specialization";
    case FunctionEntityKind::Lambda:
        return "lambda";
    case FunctionEntityKind::GenericLambdaSpecialization:
        return "generic_lambda_specialization";
    case FunctionEntityKind::Method:
        return "method";
    }
    return "unknown";
}

inline std::string semanticKindLabel(SemanticEntityKind kind) {
    switch (kind) {
    case SemanticEntityKind::Unknown:
        return "unknown";
    case SemanticEntityKind::Variable:
        return "variable";
    case SemanticEntityKind::Field:
        return "field";
    case SemanticEntityKind::Function:
        return "function";
    case SemanticEntityKind::FunctionTemplate:
        return "function_template";
    case SemanticEntityKind::Method:
        return "method";
    case SemanticEntityKind::LambdaCallOperator:
        return "lambda_call_operator";
    case SemanticEntityKind::Record:
        return "record";
    }
    return "unknown";
}

inline std::string paramDirectionLabel(pred::v2::ParamDirection direction) {
    return direction == pred::v2::ParamDirection::Output ? "output" : "input";
}

inline void applyParamPassingFromType(pred::v2::ParamDecl& param) {
    param.is_const = param.type.is_const;
    param.is_pointer = param.type.is_pointer;
    param.is_reference = param.type.is_reference;
    if (param.type.is_pointer) {
        param.passing = pred::v2::ParamPassingKind::Pointer;
    } else if (param.type.is_reference) {
        param.passing = param.type.is_const
            ? pred::v2::ParamPassingKind::ConstRef
            : pred::v2::ParamPassingKind::MutableRef;
    } else {
        param.passing = pred::v2::ParamPassingKind::Value;
    }
}

inline std::string debugLocLabel(const DebugLoc& loc) {
    if (!loc.valid()) return "<no-loc>";
    std::ostringstream os;
    os << loc.file << ":" << loc.line << ":" << loc.column;
    if (loc.end_line > 0 || loc.end_column > 0) {
        os << "-:" << loc.end_column;
    }
    return os.str();
}

inline std::string diagnosticLabel(const Diagnostic& diagnostic) {
    std::ostringstream os;
    switch (diagnostic.severity) {
    case Severity::Note:
        os << "note";
        break;
    case Severity::Warning:
        os << "warning";
        break;
    case Severity::Error:
        os << "error";
        break;
    }
    os << ": " << diagnostic.message;
    if (diagnostic.context.loc.valid()) {
        os << " [" << debugLocLabel(diagnostic.context.loc) << "]";
    }
    return os.str();
}

inline std::string debugPrintS0Clang18Step(
    const S0Clang18PipelineState& state) {
    std::ostringstream os;
    os << "S0Clang18 pipeline\n";
    os << "completed_step: " << state.completed_step;
    if (state.completed_step >= 1 && state.completed_step <= 16) {
        os << " (" << stepName(static_cast<S0Clang18Step>(state.completed_step))
           << ")";
    }
    os << "\n";
    os << "requested_stop_after: "
       << stepNumber(state.options.stop_after) << " ("
       << stepName(state.options.stop_after) << ")\n";
    os << "source: " << state.options.clang.source_name << "\n";
    os << "top_request: " << state.options.clang.top_function << "\n";

    if (state.session) {
        os << "session: main_file=" << state.session->main_file_path << "\n";
    }
    if (state.ports) {
        os << "ports: " << state.ports->ports.size() << "\n";
        for (const RawPortDecl& port : state.ports->ports) {
            os << "  - " << (port.direction == PortDirection::Input ? "input" : "output")
               << " " << port.name << ": " << typeLabel(port.type)
               << " @" << debugLocLabel(port.decl_loc) << "\n";
        }
    }
    if (state.top) {
        os << "top: " << state.top->resolved_name
           << " @" << debugLocLabel(state.top->loc) << "\n";
    }
    if (state.semantic_index) {
        os << "semantic_entities: " << state.semantic_index->entities.size() << "\n";
        for (const SemanticEntity& entity : state.semantic_index->entities) {
            os << "  - #" << entity.id << " "
               << semanticKindLabel(entity.kind) << " "
               << entity.name << " @" << debugLocLabel(entity.loc) << "\n";
        }
    }
    if (state.records) {
        os << "records: " << state.records->records.size() << "\n";
        for (const RecordMetadata& record : state.records->records) {
            os << "  - " << record.key.canonical_name
               << " fields=" << record.fields.size()
               << " ctors=" << record.constructors.size()
               << " aggregate=" << (record.aggregate_initializable ? "true" : "false")
               << " @" << debugLocLabel(record.loc) << "\n";
        }
    }
    if (state.reachability) {
        os << "reachable_functions: " << state.reachability->functions.size() << "\n";
        for (const FunctionEntity& function : state.reachability->functions) {
            os << "  - #" << function.id << " "
               << functionKindLabel(function.kind) << " "
               << function.key.stable_name;
            if (!function.key.template_values.empty()) {
                os << " template_values=[";
                for (std::size_t i = 0; i < function.key.template_values.size(); ++i) {
                    if (i > 0) os << ",";
                    os << function.key.template_values[i];
                }
                os << "]";
            }
            os << " @" << debugLocLabel(function.loc) << "\n";
        }
        os << "call_edges: " << state.reachability->call_edges.size() << "\n";
        for (const FunctionCallEdge& edge : state.reachability->call_edges) {
            os << "  - #" << edge.caller << " -> #" << edge.callee
               << " @" << debugLocLabel(edge.loc) << "\n";
        }
    }
    if (state.template_specializations) {
        os << "template_specializations: "
           << state.template_specializations->specializations.size() << "\n";
        for (const TemplateSpecializationInfo& info :
             state.template_specializations->specializations) {
            os << "  - function #" << info.function_id << " bindings=";
            if (info.value_bindings.empty()) {
                os << "[]";
            } else {
                os << "[";
                for (std::size_t i = 0; i < info.value_bindings.size(); ++i) {
                    if (i > 0) os << ", ";
                    os << info.value_bindings[i].parameter_name
                       << "=" << info.value_bindings[i].value;
                }
                os << "]";
            }
            os << " @" << debugLocLabel(info.call_loc) << "\n";
        }
    }
    if (state.lambdas) {
        os << "lambdas: " << state.lambdas->lambdas.size() << "\n";
        for (const LambdaInfo& lambda : state.lambdas->lambdas) {
            os << "  - function #" << lambda.function_id
               << " captures=[";
            for (std::size_t i = 0; i < lambda.captures.size(); ++i) {
                const LambdaCapture& capture = lambda.captures[i];
                if (i > 0) os << ", ";
                os << capture.lowered_param_name << ":";
                switch (capture.kind) {
                case LambdaCaptureKind::ByCopy:
                    os << "copy";
                    break;
                case LambdaCaptureKind::ByReference:
                    os << "ref";
                    break;
                case LambdaCaptureKind::This:
                    os << "this";
                    break;
                }
            }
            os << "] @" << debugLocLabel(lambda.loc) << "\n";
        }
    }
    if (state.surface_function) {
        const auto print_function =
            [&](const pred::v2::FunctionAST& function,
                const auto& self,
                int depth) -> void {
            std::string indent(static_cast<std::size_t>(depth * 2), ' ');
            os << indent << "function " << function.name
               << " params=" << function.params.size()
               << " stmts=" << function.body.size()
               << " helpers=" << function.helpers.size()
               << " lambdas=" << function.lambdas.size() << "\n";
            for (const pred::v2::ParamDecl& param : function.params) {
                os << indent << "  - " << paramDirectionLabel(param.direction)
                   << " " << param.name << ": " << typeLabel(param.type)
                   << " @" << debugLocLabel(param.debug_loc) << "\n";
            }
            for (const auto& helper : function.helpers) {
                if (helper) self(*helper, self, depth + 1);
            }
            for (const auto& [_, lambda] : function.lambdas) {
                if (lambda) self(*lambda, self, depth + 1);
            }
        };
        os << "surface_function:\n";
        print_function(*state.surface_function, print_function, 1);
    }
    if (state.port_lift_plan) {
        os << "port_lift_requirements: "
           << state.port_lift_plan->requirements.size() << "\n";
        for (const FunctionPortRequirement& requirement :
             state.port_lift_plan->requirements) {
            os << "  - #" << requirement.function_id << " "
               << requirement.function_name << " ports=[";
            for (std::size_t i = 0; i < requirement.ports.size(); ++i) {
                if (i > 0) os << ", ";
                os << requirement.ports[i].port_name
                   << "->" << requirement.ports[i].param_name;
            }
            os << "]\n";
        }
    }
    if (!state.diagnostics.empty()) {
        os << "diagnostics: " << state.diagnostics.size() << "\n";
        for (const Diagnostic& diagnostic : state.diagnostics) {
            os << "  - " << diagnosticLabel(diagnostic) << "\n";
        }
    }
    return os.str();
}

inline StepResult<pred::v2::FunctionAST> buildSurfaceFunctionAST(
    const S0Clang18PipelineState& state,
    const TypeLoweringContext& type_context,
    const ConstEvalContext& const_eval) {
    StepResult<pred::v2::FunctionAST> result;
    if (!state.reachability) {
        result.diagnostics.push_back(makePipelineDiagnostic(
            "s0clang18.13", {}, "S0Clang18 surface build requires reachability"));
        return result;
    }

    std::unordered_map<SemanticEntityId, std::shared_ptr<pred::v2::FunctionAST>>
        functions;
    for (const FunctionEntity& entity : state.reachability->functions) {
        auto function = std::make_shared<pred::v2::FunctionAST>();
        function->name = entity.key.stable_name;
        function->return_type = pred::v2::make_unknown_type("void");
        if (entity.function_decl) {
            DebugLoc loc = entity.loc;
            if (!loc.valid() && state.session) {
                loc = debugLocForRange(*state.session,
                                       entity.function_decl->getSourceRange(),
                                       state.options.loc_policy);
            }
            if (entity.function_decl->getReturnType()->isVoidType()) {
                function->return_type = pred::v2::make_unknown_type("void");
            } else {
                auto return_type =
                    lowerQualType(type_context, entity.function_decl->getReturnType(), loc);
                appendDiagnostics(result.diagnostics, return_type.diagnostics);
                if (return_type.ok() && return_type.value) {
                    function->return_type = return_type.value->type;
                }
            }

            if (state.lambdas) {
                if (const LambdaInfo* lambda =
                        findLambdaInfo(*state.lambdas, entity.id)) {
                    for (const LambdaCapture& capture : lambda->captures) {
                        if (capture.kind == LambdaCaptureKind::This) continue;
                        if (isLambdaObjectType(capture.type)) continue;
                        pred::v2::ParamDecl capture_param;
                        capture_param.name = capture.lowered_param_name.empty()
                            ? capture.source_name
                            : capture.lowered_param_name;
                        capture_param.type = capture.type;
                        capture_param.debug_loc = capture.loc;
                        applyParamPassingFromType(capture_param);
                        if (capture.kind == LambdaCaptureKind::ByCopy) {
                            capture_param.is_const = true;
                            capture_param.passing = pred::v2::ParamPassingKind::Value;
                        } else if (capture.kind == LambdaCaptureKind::ByReference) {
                            capture_param.is_reference = true;
                            capture_param.passing =
                                pred::v2::ParamPassingKind::MutableRef;
                        }
                        function->params.push_back(std::move(capture_param));
                    }
                }
            }

            for (const clang::ParmVarDecl* param : entity.function_decl->parameters()) {
                pred::v2::ParamDecl lowered_param;
                lowered_param.name = param ? param->getNameAsString() : "";
                lowered_param.debug_loc = state.session && param
                    ? debugLocForRange(*state.session, param->getSourceRange(),
                                       state.options.loc_policy)
                    : loc;
                if (param) {
                    auto param_type = lowerQualType(
                        type_context, param->getType(), lowered_param.debug_loc);
                    appendDiagnostics(result.diagnostics, param_type.diagnostics);
                    if (param_type.ok() && param_type.value) {
                        lowered_param.type = param_type.value->type;
                        applyParamPassingFromType(lowered_param);
                    }
                }
                function->params.push_back(std::move(lowered_param));
            }
        }

        StmtBuildContext stmt_context =
            makeStmtBuildContext(state, entity.id, type_context, const_eval);
        auto body = buildFunctionBody(stmt_context, entity.function_decl);
        appendDiagnostics(result.diagnostics, body.diagnostics);
        function->body = std::move(body.stmts);
        functions.emplace(entity.id, std::move(function));
    }

    if (hasError(result.diagnostics)) return result;

    const FunctionEntity* top_entity =
        findFunctionEntity(*state.reachability, state.reachability->top_function);
    if (!top_entity) {
        result.diagnostics.push_back(makePipelineDiagnostic(
            "s0clang18.13", {}, "S0Clang18 surface build cannot find top entity"));
        return result;
    }
    auto top_found = functions.find(top_entity->id);
    if (top_found == functions.end() || !top_found->second) {
        result.diagnostics.push_back(makePipelineDiagnostic(
            "s0clang18.13", top_entity->loc,
            "S0Clang18 surface build cannot find top function AST"));
        return result;
    }

    pred::v2::FunctionAST top = std::move(*top_found->second);
    for (const FunctionEntity& entity : state.reachability->functions) {
        if (entity.id == top_entity->id) continue;
        auto found = functions.find(entity.id);
        if (found == functions.end() || !found->second) continue;
        top.helpers.push_back(std::move(found->second));
    }
    result.value = std::move(top);
    return result;
}

inline StepResult<S0Clang18PipelineState> finishS0Clang18Run(
    S0Clang18PipelineState state) {
    StepResult<S0Clang18PipelineState> result;
    state.intermediate_text = debugPrintS0Clang18Step(state);
    result.diagnostics = state.diagnostics;
    result.value = std::move(state);
    return result;
}

template <typename T>
inline bool consumeStepResult(S0Clang18PipelineState& state,
                              StepResult<T>& step_result,
                              std::optional<T>& destination,
                              int completed_step) {
    appendDiagnostics(state.diagnostics, step_result.diagnostics);
    if (!step_result.ok()) return false;
    destination = std::move(*step_result.value);
    state.completed_step = completed_step;
    return true;
}

inline StepResult<S0Clang18PipelineState> runS0Clang18Pipeline(
    const S0Clang18PipelineOptions& options) {
    S0Clang18PipelineState state;
    state.options = options;

    int stop_after = stepNumber(options.stop_after);
    if (stop_after < 1 || stop_after > 16) {
        Diagnostic diagnostic;
        diagnostic.severity = Severity::Error;
        diagnostic.message = "S0Clang18 stop_after must be in the implemented range 1..16";
        diagnostic.context.stage = "s0clang18";
        state.diagnostics.push_back(std::move(diagnostic));
        return finishS0Clang18Run(std::move(state));
    }

    auto session = createClang18Session(options.clang);
    appendDiagnostics(state.diagnostics, session.diagnostics);
    if (!session.ok()) return finishS0Clang18Run(std::move(state));
    state.session = std::move(*session.value);
    state.completed_step = 1;
    if (stop_after <= 1) return finishS0Clang18Run(std::move(state));

    state.completed_step = 2;
    if (stop_after <= 2) return finishS0Clang18Run(std::move(state));

    auto ports = collectPragmaAndPortDecls(*state.session, options.loc_policy);
    if (!consumeStepResult(state, ports, state.ports, 3)) {
        return finishS0Clang18Run(std::move(state));
    }
    if (stop_after <= 3) return finishS0Clang18Run(std::move(state));

    auto top = selectTopFunction(*state.session, options.clang.top_function,
                                 options.loc_policy);
    if (!consumeStepResult(state, top, state.top, 4)) {
        return finishS0Clang18Run(std::move(state));
    }
    if (stop_after <= 4) return finishS0Clang18Run(std::move(state));

    auto semantic = buildSemanticIndex(*state.session, *state.top, *state.ports,
                                       options.loc_policy);
    if (!consumeStepResult(state, semantic, state.semantic_index, 5)) {
        return finishS0Clang18Run(std::move(state));
    }
    if (stop_after <= 5) return finishS0Clang18Run(std::move(state));

    TypeLoweringContext type_context = makeTypeLoweringContext(state);
    state.completed_step = 6;
    if (stop_after <= 6) return finishS0Clang18Run(std::move(state));

    ConstEvalContext const_eval = makeConstEvalContext(state, type_context);
    state.completed_step = 7;
    if (stop_after <= 7) return finishS0Clang18Run(std::move(state));

    auto records = collectRecordMetadata(*state.session, *state.top, *state.ports,
                                         type_context, options.loc_policy);
    if (!consumeStepResult(state, records, state.records, 8)) {
        return finishS0Clang18Run(std::move(state));
    }
    if (stop_after <= 8) return finishS0Clang18Run(std::move(state));

    type_context = makeTypeLoweringContext(state);
    const_eval = makeConstEvalContext(state, type_context);
    auto reachability = collectFunctionReachability(
        *state.session, *state.top, *state.semantic_index, const_eval,
        options.loc_policy);
    if (!consumeStepResult(state, reachability, state.reachability, 9)) {
        return finishS0Clang18Run(std::move(state));
    }
    if (stop_after <= 9) return finishS0Clang18Run(std::move(state));

    type_context = makeTypeLoweringContext(state);
    const_eval = makeConstEvalContext(state, type_context);
    auto templates = resolveTemplateSpecializations(
        *state.session, *state.reachability, const_eval, options.loc_policy);
    if (!consumeStepResult(state, templates, state.template_specializations, 10)) {
        return finishS0Clang18Run(std::move(state));
    }
    if (stop_after <= 10) return finishS0Clang18Run(std::move(state));

    type_context = makeTypeLoweringContext(state);
    auto lambdas = resolveLambdaCaptures(
        *state.session, *state.reachability, type_context, options.loc_policy);
    if (!consumeStepResult(state, lambdas, state.lambdas, 11)) {
        return finishS0Clang18Run(std::move(state));
    }
    if (stop_after <= 11) return finishS0Clang18Run(std::move(state));

    type_context = makeTypeLoweringContext(state);
    const_eval = makeConstEvalContext(state, type_context);
    state.completed_step = 12;
    if (stop_after <= 12) return finishS0Clang18Run(std::move(state));

    auto surface = buildSurfaceFunctionAST(state, type_context, const_eval);
    if (!consumeStepResult(state, surface, state.surface_function, 13)) {
        return finishS0Clang18Run(std::move(state));
    }
    if (stop_after <= 13) return finishS0Clang18Run(std::move(state));

    state.completed_step = 14;
    if (stop_after <= 14) return finishS0Clang18Run(std::move(state));

    state.completed_step = 15;
    if (stop_after <= 15) return finishS0Clang18Run(std::move(state));

    auto port_lift_plan = analyzeGlobalPortRequirements(
        *state.surface_function, *state.ports, *state.reachability);
    if (!consumeStepResult(state, port_lift_plan, state.port_lift_plan, 16)) {
        return finishS0Clang18Run(std::move(state));
    }
    auto lifted = liftGlobalPorts(
        std::move(*state.surface_function), *state.ports, *state.port_lift_plan);
    appendDiagnostics(state.diagnostics, lifted.diagnostics);
    if (!hasError(lifted.diagnostics)) {
        state.surface_function = std::move(lifted.function);
        state.port_lift_plan = std::move(lifted.plan);
    }

    return finishS0Clang18Run(std::move(state));
}

} // namespace pred::s0clang18
