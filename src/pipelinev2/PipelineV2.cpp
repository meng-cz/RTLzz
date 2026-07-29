#include "pipelinev2/PipelineV2.h"

#include "backend/beopt.hpp"
#include "backend/rtlgen.hpp"
#include "debug/RTLZZException.h"
#include "s0ast/S0AST.h"
#include "s0clang18/s018bridge.hpp"
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
#include <functional>
#include <optional>
#include <sstream>
#include <utility>

namespace pred::pipelinev2 {
namespace {

using TypeInfo = pred::v2::TypeInfo;
using ParamDirection = pred::v2::ParamDirection;
using ParamPassingKind = pred::v2::ParamPassingKind;
using pred::v2::paramDirectionName;

using DebugTextProvider = std::function<std::string()>;
using DebugSignalsProvider = std::function<std::vector<rtlgen::RtlDebugSignal>()>;

bool sameFileOrUnknown(const std::string& lhs, const std::string& rhs) {
    return lhs.empty() || rhs.empty() || lhs == rhs;
}

bool locContains(const DebugLoc& container, const DebugLoc& loc) {
    if (!container.valid() || !loc.valid()) return false;
    if (!sameFileOrUnknown(container.file, loc.file)) return false;
    if (container.line <= 0 || loc.line <= 0) return false;
    int end_line = container.end_line > 0 ? container.end_line : container.line;
    int end_column = container.end_column > 0 ? container.end_column : container.column;
    if (loc.line < container.line || loc.line > end_line) return false;
    if (loc.line == container.line && container.column > 0 && loc.column > 0 &&
        loc.column < container.column) {
        return false;
    }
    if (loc.line == end_line && end_column > 0 && loc.column > 0 &&
        loc.column > end_column) {
        return false;
    }
    return true;
}

bool locMatches(const DebugLoc& lhs, const DebugLoc& rhs) {
    if (!lhs.valid() || !rhs.valid()) return false;
    if (!sameFileOrUnknown(lhs.file, rhs.file)) return false;
    if (lhs.line > 0 && rhs.line > 0 && lhs.line == rhs.line) return true;
    return locContains(lhs, rhs) || locContains(rhs, lhs);
}

bool signalMatchesLoc(const rtlgen::RtlDebugSignal& signal, const DebugLoc& loc) {
    if (!loc.valid()) return false;
    if (signal.decl_loc && locMatches(*signal.decl_loc, loc)) return true;
    for (const auto& candidate : signal.primary_locs) {
        if (locMatches(candidate, loc)) return true;
    }
    for (const auto& candidate : signal.related_locs) {
        if (locMatches(candidate, loc)) return true;
    }
    return false;
}

std::string locText(const DebugLoc& loc) {
    if (!loc.valid()) return "<none>";
    std::ostringstream os;
    if (!loc.file.empty()) os << loc.file;
    if (loc.line > 0) {
        if (!loc.file.empty()) os << ":";
        os << loc.line;
        if (loc.column > 0) os << ":" << loc.column;
    }
    if (loc.end_line > 0 || loc.end_column > 0) {
        os << "-";
        if (loc.end_line > 0) os << loc.end_line;
        if (loc.end_column > 0) os << ":" << loc.end_column;
    }
    return os.str();
}

void emitLocList(std::ostream& os, const std::vector<DebugLoc>& locs) {
    bool any = false;
    for (const auto& loc : locs) {
        if (!loc.valid()) continue;
        os << "    - " << locText(loc) << "\n";
        any = true;
    }
    if (!any) os << "    - <none>\n";
}

std::string signalDebugText(const std::vector<rtlgen::RtlDebugSignal>& signals) {
    if (signals.empty()) return {};
    std::ostringstream os;
    os << "related signal debug\n";
    for (const auto& signal : signals) {
        os << "- #" << signal.id << " " << signal.signal_name << "\n";
        os << "  rtl_line: " << signal.rtl_line << "\n";
        if (!signal.port_name.empty()) {
            os << "  port: " << signal.port_name;
            if (signal.port_element_index >= 0) os << "[" << signal.port_element_index << "]";
            os << "\n";
        }
        os << "  decl:\n";
        if (signal.decl_loc && signal.decl_loc->valid()) os << "    - " << locText(*signal.decl_loc) << "\n";
        else os << "    - <none>\n";
        os << "  primary:\n";
        emitLocList(os, signal.primary_locs);
        os << "  related:\n";
        emitLocList(os, signal.related_locs);
        os << "  messages:\n";
        if (signal.messages.empty()) {
            os << "    - <none>\n";
        } else {
            for (const auto& message : signal.messages) os << "    - " << message << "\n";
        }
        if (!signal.derived_nodes.empty()) {
            os << "  derived_nodes:\n";
            for (const auto& node : signal.derived_nodes) {
                os << "    - #" << node.id;
                if (!node.name.empty()) os << " " << node.name;
                os << "\n";
            }
        }
        if (!signal.derived_names.empty()) {
            os << "  derived_names:\n";
            for (const auto& name : signal.derived_names) os << "    - " << name << "\n";
        }
    }
    return os.str();
}

template <typename ErrorT>
std::optional<ErrorContext> stageContext(const std::optional<ErrorT>& error) {
    if (!error) return std::nullopt;
    return error->context;
}

PipelineResult errorResult(std::string stage,
                           std::string message,
                           std::optional<ErrorContext> context = std::nullopt,
                           const DebugTextProvider& debug_text = {},
                           const DebugSignalsProvider& debug_signals = {}) {
    PipelineResult result;
    result.error = std::move(stage) + ": " + std::move(message);
    if (debug_text) result.error_debug_text = debug_text();
    if (debug_signals) {
        result.error_rtl_debug_signals = debug_signals();
        if (context && context->loc.valid()) {
            for (const auto& signal : result.error_rtl_debug_signals) {
                if (signalMatchesLoc(signal, context->loc)) {
                    result.error_signal_debug_signals.push_back(signal);
                }
            }
        }
        result.error_signal_debug_text = signalDebugText(result.error_signal_debug_signals);
    }
    return result;
}

std::string stageError(const std::optional<s1apinorm::APINormError>& error) {
    return error ? error->formatted : "stage failed";
}

std::string stageError(const std::optional<s0ast::S0Diagnostic>& error) {
    return error ? error->message : "stage failed";
}

std::string stageError(const std::optional<s0clang18::Diagnostic>& error) {
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

TypeInfo sourceScalarTypeForMetadata(const s7flatten::S7PortGroup& group) {
    TypeInfo type = group.source_type;
    type.is_array = false;
    type.array_size = 0;
    type.array_dims.clear();
    if (type.width <= 0) {
        type.width = group.scalar_type.width;
    }
    if (type.name.empty()) {
        type.name = group.scalar_type.name;
    }
    if (type.hw_kind.empty()) {
        type.hw_kind = group.scalar_type.hw_kind;
    }
    return type;
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
        emitTypeMetadata(os, sourceScalarTypeForMetadata(group), group.array_dims);
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

    DebugTextProvider current_debug_text;
    DebugSignalsProvider current_debug_signals;
    s0clang18::Clang18BuildResult s0;
    s1apinorm::APINormResult s1;
    s2validate::ValidateResult s2;
    s3statementize::StatementizeResult s3;
    s4cfg::CFGResult s4;
    s5unroll::UnrollResult s5;
    s6inline::InlineResult s6;
    s7flatten::FlattenResult s7;
    s8opnorm::NormResult s8;
    s9ssa::SSAResult s9;
    s10predicate::PredicateResult s10;
    s11beir::BEIRResult s11;
    beir::Program beir_program;
    try {
        s0clang18::Clang18Options clang18_options;
        clang18_options.source_name = config.source_name;
        clang18_options.source_text = config.source_text;
        clang18_options.top_function = config.top_function;
        clang18_options.clang_args = config.clang_args;
        s0 = s0clang18::buildS0ProgramWithClang18(clang18_options);
        if (!s0.ok()) return errorResult("s0clang18", stageError(s0.error), stageContext(s0.error));
        if (!s0.program) return errorResult("s0clang18", "stage produced no program");
        current_debug_text = [&s0]() {
            return "latest successful stage: s0clang18\n" +
                   s0ast::debugPrint(*s0.program);
        };
        s1 = s1apinorm::normalizeAPIs(s0ast::surfaceAST(*s0.program));
        if (!s1.ok()) return errorResult("s1apinorm", stageError(s1.error), stageContext(s1.error), current_debug_text);
        if (!s1.function) return errorResult("s1apinorm", "stage produced no function", std::nullopt, current_debug_text);
        current_debug_text = [&s1]() { return "latest successful stage: s1apinorm\n" + s1apinorm::debugPrint(*s1.function, s1.summaries); };

        s2 = s2validate::validateFunctionAST(*s1.function);
        if (!s2.ok()) return errorResult("s2validate", stageError(s2.error), stageContext(s2.error), current_debug_text);

        s3 = s3statementize::statementizeFunctionAST(*s1.function);
        if (!s3.ok()) return errorResult("s3statementize", stageError(s3.error), stageContext(s3.error), current_debug_text);
        if (!s3.program) return errorResult("s3statementize", "stage produced no program", std::nullopt, current_debug_text);
        current_debug_text = [&s3]() { return "latest successful stage: s3statementize\n" + s3statementize::debugPrint(*s3.program); };

        s4 = s4cfg::buildCFGProgram(*s3.program);
        if (!s4.ok()) return errorResult("s4cfg", stageError(s4.error), stageContext(s4.error), current_debug_text);
        if (!s4.program) return errorResult("s4cfg", "stage produced no program", std::nullopt, current_debug_text);
        current_debug_text = [&s4]() { return "latest successful stage: s4cfg\n" + s4cfg::debugPrint(*s4.program); };

        s5unroll::UnrollOptions unroll_options;
        unroll_options.max_iterations_per_loop = config.unroll_limit;
        s5 = s5unroll::unrollCFGProgram(*s4.program, unroll_options);
        if (!s5.ok()) return errorResult("s5unroll", stageError(s5.error), stageContext(s5.error), current_debug_text);
        if (!s5.program) return errorResult("s5unroll", "stage produced no program", std::nullopt, current_debug_text);
        current_debug_text = [&s5]() { return "latest successful stage: s5unroll\n" + s5unroll::debugPrint(*s5.program, s5.summaries); };

        s6 = s6inline::inlineCFGProgram(*s5.program);
        if (!s6.ok()) return errorResult("s6inline", stageError(s6.error), stageContext(s6.error), current_debug_text);
        if (!s6.program) return errorResult("s6inline", "stage produced no program", std::nullopt, current_debug_text);
        current_debug_text = [&s6]() { return "latest successful stage: s6inline\n" + s6inline::debugPrint(*s6.program, s6.summaries); };

        s7 = s7flatten::flattenProgram(*s6.program);
        if (!s7.ok()) return errorResult("s7flatten", stageError(s7.error), stageContext(s7.error), current_debug_text);
        if (!s7.program) return errorResult("s7flatten", "stage produced no program", std::nullopt, current_debug_text);
        current_debug_text = [&s7]() { return "latest successful stage: s7flatten\n" + s7flatten::debugPrint(*s7.program, s7.summaries); };

        if (config.output_kind == OutputKind::PortMetadata) {
            PipelineResult result;
            result.output_text = portMetadataJson(*s7.program);
            return result;
        }

        s8 = s8opnorm::normalizeOperations(*s7.program);
        if (!s8.ok()) return errorResult("s8opnorm", stageError(s8.error), stageContext(s8.error), current_debug_text);
        if (!s8.program) return errorResult("s8opnorm", "stage produced no program", std::nullopt, current_debug_text);
        current_debug_text = [&s8]() { return "latest successful stage: s8opnorm\n" + s8opnorm::debugPrint(*s8.program, s8.summaries); };

        s9 = s9ssa::buildSSA(*s8.program);
        if (!s9.ok()) return errorResult("s9ssa", stageError(s9.error), stageContext(s9.error), current_debug_text);
        if (!s9.program) return errorResult("s9ssa", "stage produced no program", std::nullopt, current_debug_text);
        current_debug_text = [&s9]() { return "latest successful stage: s9ssa\n" + s9ssa::debugPrint(*s9.program, s9.summaries); };

        s10 = s10predicate::lowerPredicates(*s9.program);
        if (!s10.ok()) return errorResult("s10predicate", stageError(s10.error), stageContext(s10.error), current_debug_text);
        if (!s10.program) return errorResult("s10predicate", "stage produced no program", std::nullopt, current_debug_text);
        current_debug_text = [&s10]() { return "latest successful stage: s10predicate\n" + s10predicate::debugPrint(*s10.program, s10.summaries); };

        s11beir::BEIROptions beir_options;
        beir_options.optimize = false;
        s11 = s11beir::buildBEIR(*s10.program, beir_options);
        if (!s11.ok()) return errorResult("s11beir", stageError(s11.error), stageContext(s11.error), current_debug_text);
        if (!s11.program) return errorResult("s11beir", "stage produced no program", std::nullopt, current_debug_text);
        current_debug_text = [&s11]() { return "latest successful stage: s11beir\n" + beir::emitText(*s11.program); };
        current_debug_signals = [&s11]() { return rtlgen::collectDebugSignals(*s11.program, ""); };

        beir_program = *s11.program;
        beir_program = beir::opt::optimizeProgram(
            std::move(beir_program),
            beir::opt::parseOptions(config.beopt_args));
        current_debug_text = [&beir_program]() {
            return "latest successful stage: beir-opt\n" + beir::emitText(beir_program);
        };
        current_debug_signals = [&beir_program]() {
            return rtlgen::collectDebugSignals(beir_program, "");
        };

        PipelineResult result;
        switch (config.output_kind) {
        case OutputKind::Beir:
            result.output_text = beir::emitText(beir_program);
            break;
        case OutputKind::Rtl:
            result.output_text = rtlgen::emitSystemVerilog(beir_program);
            if (config.rtl_debug_output == RtlDebugOutputKind::Structured) {
                result.rtl_debug_signals =
                    rtlgen::collectDebugSignals(beir_program, result.output_text);
            } else if (config.rtl_debug_output == RtlDebugOutputKind::Text) {
                result.rtl_debug_text = rtlgen::emitDebugReport(beir_program,
                                                                result.output_text);
            }
            break;
        case OutputKind::PortMetadata:
            break;
        }
        result.beir_program = std::move(beir_program);
        return result;
    } catch (const RTLZZException& ex) {
        return errorResult("pipelinev2",
                           ex.what(),
                           ex.primaryContext(),
                           current_debug_text,
                           current_debug_signals);
    } catch (const std::exception& ex) {
        return errorResult("pipelinev2", ex.what(), std::nullopt, current_debug_text, current_debug_signals);
    }
}

} // namespace pred::pipelinev2
