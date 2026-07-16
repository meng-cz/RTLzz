#include "pipelinev2/PipelineV2.h"

#include "ast/ASTBuilder.h"
#include "backend/beopt.hpp"
#include "backend/rtlgen.hpp"
#include "s1apinorm/S1APINorm.h"
#include "s2validate/S2Validate.h"
#include "s3statementize/S3Statementize.h"
#include "s4cfg/S4CFG.h"
#include "s5unroll/S5Unroll.h"
#include "s6inline/S6Inline.h"
#include "s7flatten/S7Flatten.h"
#include "s8opnorm/S8Norm.h"
#include "s9ssa/S9SSA.h"
#include "s10predicate/S10Predicate.h"
#include "s11beir/S11BEIR.h"

#include <exception>
#include <utility>

namespace pred::pipelinev2 {
namespace {

PipelineResult errorResult(std::string stage, std::string message) {
    PipelineResult result;
    result.error = std::move(stage) + ": " + std::move(message);
    return result;
}

std::string stageError(const std::optional<s1apinorm::APINormError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s2validate::ValidateError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s3statementize::StatementizeError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s4cfg::CFGError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s5unroll::UnrollError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s6inline::InlineError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s7flatten::FlattenError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s8opnorm::NormError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s9ssa::SSABuildError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s10predicate::PredicateError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s11beir::BEIRError>& error) {
    return error ? error->formatted : "stage failed";
}

} // namespace

PipelineResult compile(const PipelineConfig& config) {
    if (config.source_name.empty()) return errorResult("config", "source_name must not be empty");
    if (config.top_function.find_first_not_of(" \t\r\n") == std::string::npos) {
        return errorResult("config", "top_function must not be empty");
    }
    if (config.unroll_limit <= 0) return errorResult("config", "unroll_limit must be positive");

    try {
        BuildResult parsed;
        if (config.source_text) {
            parsed = buildASTFromSourceText(config.source_name,
                                            *config.source_text,
                                            config.top_function,
                                            config.clang_args);
        } else {
            parsed = buildASTFromSource(config.source_name,
                                        config.top_function,
                                        config.clang_args);
        }
        if (!parsed.error.empty()) return errorResult("parse", std::move(parsed.error));
        if (!parsed.function) return errorResult("parse", "failed to extract function");

        auto s1 = s1apinorm::normalizeAPIs(*parsed.function);
        if (!s1.ok()) return errorResult("s1apinorm", stageError(s1.error));
        if (!s1.function) return errorResult("s1apinorm", "stage produced no function");

        auto s2 = s2validate::validateFunctionAST(*s1.function);
        if (!s2.ok()) return errorResult("s2validate", stageError(s2.error));

        auto s3 = s3statementize::statementizeFunctionAST(*s1.function);
        if (!s3.ok()) return errorResult("s3statementize", stageError(s3.error));
        if (!s3.program) return errorResult("s3statementize", "stage produced no program");

        auto s4 = s4cfg::buildCFGProgram(*s3.program);
        if (!s4.ok()) return errorResult("s4cfg", stageError(s4.error));
        if (!s4.program) return errorResult("s4cfg", "stage produced no program");

        s5unroll::UnrollOptions unroll_options;
        unroll_options.max_iterations_per_loop = config.unroll_limit;
        auto s5 = s5unroll::unrollCFGProgram(*s4.program, unroll_options);
        if (!s5.ok()) return errorResult("s5unroll", stageError(s5.error));
        if (!s5.program) return errorResult("s5unroll", "stage produced no program");

        auto s6 = s6inline::inlineCFGProgram(*s5.program);
        if (!s6.ok()) return errorResult("s6inline", stageError(s6.error));
        if (!s6.program) return errorResult("s6inline", "stage produced no program");

        auto s7 = s7flatten::flattenProgram(*s6.program);
        if (!s7.ok()) return errorResult("s7flatten", stageError(s7.error));
        if (!s7.program) return errorResult("s7flatten", "stage produced no program");

        auto s8 = s8opnorm::normalizeOperations(*s7.program);
        if (!s8.ok()) return errorResult("s8opnorm", stageError(s8.error));
        if (!s8.program) return errorResult("s8opnorm", "stage produced no program");

        auto s9 = s9ssa::buildSSA(*s8.program);
        if (!s9.ok()) return errorResult("s9ssa", stageError(s9.error));
        if (!s9.program) return errorResult("s9ssa", "stage produced no program");

        auto s10 = s10predicate::lowerPredicates(*s9.program);
        if (!s10.ok()) return errorResult("s10predicate", stageError(s10.error));
        if (!s10.program) return errorResult("s10predicate", "stage produced no program");

        s11beir::BEIROptions beir_options;
        beir_options.optimize = false;
        auto s11 = s11beir::buildBEIR(*s10.program, beir_options);
        if (!s11.ok()) return errorResult("s11beir", stageError(s11.error));
        if (!s11.program) return errorResult("s11beir", "stage produced no program");

        beir::Program beir_program = beir::opt::optimizeProgram(
            std::move(*s11.program),
            beir::opt::parseOptions(config.beopt_args));

        PipelineResult result;
        switch (config.output_kind) {
        case OutputKind::Beir:
            result.output_text = beir::emitText(beir_program);
            break;
        case OutputKind::Rtl:
            result.output_text = rtlgen::emitSystemVerilog(beir_program);
            break;
        }
        result.beir_program = std::move(beir_program);
        return result;
    } catch (const std::exception& ex) {
        return errorResult("pipelinev2", ex.what());
    }
}

} // namespace pred::pipelinev2
