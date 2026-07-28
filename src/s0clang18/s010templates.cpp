#include "s0clang18/s010templates.hpp"

#include <clang/AST/DeclTemplate.h>

#include <algorithm>
#include <optional>
#include <sstream>

namespace pred::s0clang18 {
namespace {

Diagnostic makeError(const Clang18Session& session,
                     DebugLoc loc,
                     std::string message) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Error;
    diagnostic.message = std::move(message);
    diagnostic.context.stage = "s0clang18.10";
    diagnostic.context.source_file = session.main_file_path;
    diagnostic.context.loc = std::move(loc);
    return diagnostic;
}

bool hasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [](const Diagnostic& diagnostic) {
                           return diagnostic.severity == Severity::Error;
                       });
}

std::string functionName(const clang::FunctionDecl* function) {
    if (!function) return "<unknown>";
    std::string qualified = function->getQualifiedNameAsString();
    if (!qualified.empty()) return qualified;
    std::string name = function->getNameAsString();
    if (!name.empty()) return name;
    return "<anonymous>";
}

std::string parameterName(const clang::NamedDecl* parameter, unsigned index) {
    if (!parameter) return "arg" + std::to_string(index);
    std::string name = parameter->getNameAsString();
    if (!name.empty()) return name;
    return "arg" + std::to_string(index);
}

const clang::FunctionTemplateDecl* primaryFunctionTemplate(
    const clang::FunctionDecl* function) {
    if (!function) return nullptr;
    if (const clang::FunctionTemplateDecl* primary = function->getPrimaryTemplate()) {
        return primary;
    }
    if (const clang::FunctionTemplateDecl* described =
            function->getDescribedFunctionTemplate()) {
        return described;
    }
    return nullptr;
}

const clang::TemplateArgumentList* specializationArgs(
    const clang::FunctionDecl* function) {
    if (!function) return nullptr;
    return function->getTemplateSpecializationArgs();
}

DebugLoc firstIncomingCallLoc(const FunctionReachabilityGraph& reachability,
                              SemanticEntityId function_id,
                              DebugLoc fallback) {
    for (const FunctionCallEdge& edge : reachability.call_edges) {
        if (edge.callee == function_id && edge.loc.valid()) return edge.loc;
    }
    return fallback;
}

void appendBindingForArgument(const Clang18Session& session,
                              const ConstEvalContext& const_eval,
                              const clang::NamedDecl* parameter,
                              unsigned parameter_index,
                              const clang::TemplateArgument& argument,
                              DebugLoc loc,
                              TemplateSpecializationInfo& info,
                              std::vector<Diagnostic>& diagnostics) {
    auto value = evalTemplateIntegralArgument(const_eval, argument, loc);
    if (!value.ok()) {
        diagnostics.insert(diagnostics.end(),
                           value.diagnostics.begin(),
                           value.diagnostics.end());
        return;
    }
    if (!value.value) {
        diagnostics.push_back(makeError(
            session, loc,
            "Unable to evaluate non-type template parameter '" +
                parameterName(parameter, parameter_index) + "'"));
        return;
    }

    TemplateValueBinding binding;
    binding.parameter_name = parameterName(parameter, parameter_index);
    binding.value = *value.value;
    binding.loc = loc;
    info.value_bindings.push_back(std::move(binding));
}

std::optional<TemplateSpecializationInfo> resolveOneSpecialization(
    const Clang18Session& session,
    const FunctionReachabilityGraph& reachability,
    const ConstEvalContext& const_eval,
    const FunctionEntity& function,
    const SourceLocPolicy& loc_policy,
    std::vector<Diagnostic>& diagnostics) {
    const clang::FunctionDecl* specialization = function.function_decl;
    const clang::FunctionTemplateDecl* primary =
        primaryFunctionTemplate(specialization);
    const clang::TemplateArgumentList* args = specializationArgs(specialization);
    if (!primary || !args) return std::nullopt;

    DebugLoc call_loc =
        firstIncomingCallLoc(reachability, function.id, function.loc);

    TemplateSpecializationInfo info;
    info.function_id = function.id;
    info.specialization_decl = specialization;
    info.primary_template_decl = primary;
    info.call_loc = call_loc;

    const clang::TemplateParameterList* parameters =
        primary->getTemplateParameters();
    if (!parameters) {
        diagnostics.push_back(makeError(
            session, call_loc,
            "Template specialization for '" + functionName(specialization) +
                "' has no primary template parameter list"));
        return info;
    }

    llvm::ArrayRef<clang::TemplateArgument> arg_array = args->asArray();
    unsigned param_count = parameters->size();
    unsigned arg_count = arg_array.size();
    if (arg_count < param_count) {
        diagnostics.push_back(makeError(
            session, call_loc,
            "Template specialization for '" + functionName(specialization) +
                "' has fewer arguments than template parameters"));
    }

    unsigned count = std::min(param_count, arg_count);
    for (unsigned index = 0; index < count; ++index) {
        const clang::NamedDecl* parameter = parameters->getParam(index);
        const auto* non_type =
            llvm::dyn_cast_or_null<clang::NonTypeTemplateParmDecl>(parameter);
        if (!non_type) continue;

        const clang::TemplateArgument& argument = arg_array[index];
        DebugLoc binding_loc = debugLocForRange(
            session, non_type->getSourceRange(), loc_policy);
        if (!binding_loc.valid()) binding_loc = call_loc;

        if (argument.getKind() == clang::TemplateArgument::Pack) {
            unsigned pack_index = 0;
            for (const clang::TemplateArgument& packed : argument.pack_elements()) {
                TemplateSpecializationInfo packed_info;
                appendBindingForArgument(session, const_eval, parameter, index,
                                         packed, binding_loc, packed_info,
                                         diagnostics);
                for (TemplateValueBinding& binding : packed_info.value_bindings) {
                    binding.parameter_name += "_" + std::to_string(pack_index++);
                    info.value_bindings.push_back(std::move(binding));
                }
            }
            continue;
        }

        appendBindingForArgument(session, const_eval, parameter, index,
                                 argument, binding_loc, info, diagnostics);
    }

    return info;
}

void addSpecialization(TemplateSpecializationTable& table,
                       TemplateSpecializationInfo info) {
    std::size_t index = table.specializations.size();
    table.specialization_by_function[info.function_id] = index;
    table.specializations.push_back(std::move(info));
}

} // namespace

StepResult<TemplateSpecializationTable> resolveTemplateSpecializations(
    const Clang18Session& session,
    const FunctionReachabilityGraph& reachability,
    const ConstEvalContext& const_eval,
    const SourceLocPolicy& loc_policy) {
    StepResult<TemplateSpecializationTable> result;
    if (!session.ast_context || !session.translation_unit) {
        result.diagnostics.push_back(makeError(
            session, {}, "S0Clang18 template resolve requires a valid Clang18Session"));
        return result;
    }

    TemplateSpecializationTable table;
    for (const FunctionEntity& function : reachability.functions) {
        auto info = resolveOneSpecialization(
            session, reachability, const_eval, function, loc_policy,
            result.diagnostics);
        if (!info) continue;
        addSpecialization(table, std::move(*info));
    }

    if (hasError(result.diagnostics)) return result;
    result.value = std::move(table);
    return result;
}

const TemplateSpecializationInfo* findTemplateSpecialization(
    const TemplateSpecializationTable& table,
    SemanticEntityId function_id) {
    auto found = table.specialization_by_function.find(function_id);
    if (found == table.specialization_by_function.end()) return nullptr;
    if (found->second >= table.specializations.size()) return nullptr;
    return &table.specializations[found->second];
}

} // namespace pred::s0clang18
