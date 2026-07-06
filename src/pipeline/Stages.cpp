#include "pipeline/Stages.h"

#include "ast/ASTBuilder.h"

#include <utility>

namespace pred::pipeline {

ParseResult parseSource(const ParseConfig& config) {
    auto build = pred::buildASTFromSource(config.source_file,
                                          config.top_function,
                                          config.clang_args);
    ParseResult result;
    result.function = std::move(build.function);
    result.error = std::move(build.error);
    return result;
}

UnrollStageResult unrollFunction(FunctionAST function,
                                 const UnrollConfig& config) {
    auto unroll_body = [&](std::vector<StmtPtr>& body,
                           const std::string& label) -> std::string {
        auto unrolled = pred::unrollLoops(body, config);
        if (!unrolled.error.empty()) {
            return "Failed to unroll " + label + ": " + unrolled.error;
        }
        body = std::move(unrolled.body);
        return {};
    };

    if (auto error = unroll_body(function.body, "top function '" + function.name + "'");
        !error.empty()) {
        return {{}, error};
    }

    for (auto& [name, lambda] : function.lambdas) {
        if (!lambda) continue;
        if (auto error = unroll_body(lambda->body, "lambda '" + name + "'");
            !error.empty()) {
            return {{}, error};
        }
    }

    for (auto& helper : function.helpers) {
        if (!helper) continue;
        if (auto error = unroll_body(helper->body, "helper '" + helper->name + "'");
            !error.empty()) {
            return {{}, error};
        }
    }

    UnrollStageResult result;
    result.function = std::move(function);
    return result;
}

InlineStageResult inlineHelpersAndLambdas(FunctionAST function) {
    auto inlined = pred::inlineHelpersAndLambdas(function, function.body);
    if (!inlined.error.empty()) {
        return {{}, std::move(inlined.error)};
    }
    function.body = std::move(inlined.body);
    InlineStageResult result;
    result.function = std::move(function);
    return result;
}

NormalizeResult normalizeFunction(const FunctionAST& function) {
    return pred::normalizeFunction(function, function.body);
}

CFGStageResult buildControlFlow(const NormalizeResult& normalized) {
    CFGStageResult result;
    if (!normalized.error.empty()) {
        result.error = normalized.error;
        return result;
    }
    result.cfg = pred::buildCFG(normalized.body);
    return result;
}

SSAStageResult buildSSAForm(const CFGStageResult& cfg,
                            const NormalizeResult& normalized) {
    SSAStageResult result;
    if (!cfg.error.empty()) {
        result.error = cfg.error;
        return result;
    }
    result.program = pred::buildSSA(cfg.cfg, normalized.ssa_seed_symbols);
    result.error = result.program.error;
    return result;
}

} // namespace pred::pipeline
