#include "pipelinev2/PipelineV2.h"

#include "backend/beopt.hpp"
#include "backend/rtlgen.hpp"
#include "s0ast/S0AST.h"
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
#include <sstream>
#include <utility>

namespace pred::pipelinev2 {
namespace {

using TypeInfo = pred::v2::TypeInfo;
using ParamDirection = pred::v2::ParamDirection;
using ParamPassingKind = pred::v2::ParamPassingKind;
using pred::v2::paramDirectionName;

PipelineResult errorResult(std::string stage, std::string message) {
    PipelineResult result;
    result.error = std::move(stage) + ": " + std::move(message);
    return result;
}

std::string stageError(const std::optional<s1apinorm::APINormError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s0ast::S0Diagnostic>& error) {
    return error ? error->message : "stage failed";
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

std::string jsonEscape(const std::string& text) {
    std::ostringstream os;
    for (char ch : text) {
        switch (ch) {
        case '"': os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        case '\b': os << "\\b"; break;
        case '\f': os << "\\f"; break;
        case '\n': os << "\\n"; break;
        case '\r': os << "\\r"; break;
        case '\t': os << "\\t"; break;
        default:
            unsigned char c = static_cast<unsigned char>(ch);
            if (c < 0x20) {
                os << "\\u";
                constexpr char hex[] = "0123456789abcdef";
                os << "00" << hex[(c >> 4) & 0xf] << hex[c & 0xf];
            } else {
                os << ch;
            }
            break;
        }
    }
    return os.str();
}

void emitJsonString(std::ostream& os, const std::string& text) {
    os << '"' << jsonEscape(text) << '"';
}

void emitIntArray(std::ostream& os, const std::vector<int>& values) {
    os << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) os << ", ";
        os << values[i];
    }
    os << "]";
}

std::string passingName(ParamPassingKind passing) {
    switch (passing) {
    case ParamPassingKind::Value: return "Value";
    case ParamPassingKind::ConstRef: return "ConstRef";
    case ParamPassingKind::MutableRef: return "MutableRef";
    case ParamPassingKind::RValueRef: return "RValueRef";
    case ParamPassingKind::Pointer: return "Pointer";
    }
    return "Value";
}

std::string typeNameForMetadata(const TypeInfo& type) {
    if (!type.name.empty()) return type.name;
    if (type.hw_kind == "bool") return "bool";
    if (type.hw_kind == "Int" && type.width > 0) {
        return "Int<" + std::to_string(type.width) + ">";
    }
    if (type.hw_kind == "UInt" && type.width > 0) {
        return "UInt<" + std::to_string(type.width) + ">";
    }
    if (!type.hw_kind.empty()) return type.hw_kind;
    return "bits";
}

void emitTypeMetadata(std::ostream& os,
                      const TypeInfo& scalar_type,
                      const std::vector<int>& array_dims) {
    os << "{";
    os << "\"name\": ";
    emitJsonString(os, typeNameForMetadata(scalar_type));
    os << ", \"width\": " << scalar_type.width;
    os << ", \"signed\": " << (scalar_type.is_signed ? "true" : "false");
    os << ", \"hw_kind\": ";
    emitJsonString(os, scalar_type.hw_kind);
    os << ", \"is_array\": " << (!array_dims.empty() ? "true" : "false");
    os << ", \"array_size\": " << (array_dims.empty() ? 0 : array_dims.front());
    os << ", \"array_dims\": ";
    emitIntArray(os, array_dims);
    os << "}";
}

std::string portMetadataJson(const s7flatten::S7FlattenedProgram& program) {
    const auto& fn = program.top;
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema_version\": \"rtlzz-pipelinev2-portmeta-v1\",\n";
    os << "  \"function\": ";
    emitJsonString(os, fn.name);
    os << ",\n";
    os << "  \"ports\": [\n";
    for (std::size_t i = 0; i < fn.port_groups.size(); ++i) {
        const auto& group = fn.port_groups[i];
        os << "    {\n";
        os << "      \"name\": ";
        emitJsonString(os, group.source_name);
        os << ",\n";
        os << "      \"direction\": ";
        emitJsonString(os, paramDirectionName(group.direction));
        os << ",\n";
        os << "      \"passing\": ";
        emitJsonString(os, passingName(group.passing));
        os << ",\n";
        os << "      \"type\": ";
        emitTypeMetadata(os, group.scalar_type, group.array_dims);
        os << ",\n";
        os << "      \"element_symbols\": [";
        for (std::size_t j = 0; j < group.elements.size(); ++j) {
            if (j) os << ", ";
            const auto& element = group.elements[j];
            std::string name = "port_" + std::to_string(element.symbol);
            if (element.symbol >= 0 &&
                element.symbol < static_cast<s7flatten::SymbolId>(fn.symbols.size())) {
                name = fn.symbols[static_cast<std::size_t>(element.symbol)].debug_name;
            }
            emitJsonString(os, name);
        }
        os << "],\n";
        os << "      \"elements\": [";
        for (std::size_t j = 0; j < group.elements.size(); ++j) {
            if (j) os << ", ";
            const auto& element = group.elements[j];
            std::string name = "port_" + std::to_string(element.symbol);
            if (element.symbol >= 0 &&
                element.symbol < static_cast<s7flatten::SymbolId>(fn.symbols.size())) {
                name = fn.symbols[static_cast<std::size_t>(element.symbol)].debug_name;
            }
            os << "{\"symbol\": ";
            emitJsonString(os, name);
            os << ", \"indices\": ";
            emitIntArray(os, element.indices);
            os << "}";
        }
        os << "]\n";
        os << "    }";
        if (i + 1 < fn.port_groups.size()) os << ",";
        os << "\n";
    }
    os << "  ]\n";
    os << "}\n";
    return os.str();
}

} // namespace

PipelineResult compile(const PipelineConfig& config) {
    if (config.source_name.empty()) return errorResult("config", "source_name must not be empty");
    if (config.top_function.find_first_not_of(" \t\r\n") == std::string::npos) {
        return errorResult("config", "top_function must not be empty");
    }
    if (config.unroll_limit <= 0) return errorResult("config", "unroll_limit must be positive");

    try {
        auto s0 = s0ast::parseProgram(config.source_name,
                                      config.source_text,
                                      config.top_function,
                                      config.clang_args);
        if (!s0.ok()) return errorResult("s0ast", stageError(s0.error));
        if (!s0.program) return errorResult("s0ast", "stage produced no program");
        auto s1 = s1apinorm::normalizeAPIs(s0ast::surfaceAST(*s0.program));
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

        if (config.output_kind == OutputKind::PortMetadata) {
            PipelineResult result;
            result.output_text = portMetadataJson(*s7.program);
            return result;
        }

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
        case OutputKind::PortMetadata:
            break;
        }
        result.beir_program = std::move(beir_program);
        return result;
    } catch (const std::exception& ex) {
        return errorResult("pipelinev2", ex.what());
    }
}

} // namespace pred::pipelinev2
