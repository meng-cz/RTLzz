#include "ast/ASTBuilder.h"
#include "ir/CFG.h"
#include "ir/SSA.h"
#include "transform/LoopUnroll.h"
#include "transform/Normalize.h"
#include "transform/Predicate.h"
#include "predicate/OutputExpressionMap.h"
#include "predicate/PredicateVerifier.h"
#include "emitter/ListJsonEmitter.h"

#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " <source.cpp> --top <function_name> [--format listjson]"
              << " [--input source.cpp] [--unroll-limit N] [--clang-arg ARG ...] [-o output_file]\n";
}

static std::vector<std::string> splitArgs(const std::string& text) {
    std::vector<std::string> args;
    std::string current;
    bool in_quote = false;
    char quote = 0;
    for (char ch : text) {
        if ((ch == '"' || ch == '\'') && (!in_quote || ch == quote)) {
            if (in_quote) {
                in_quote = false;
                quote = 0;
            } else {
                in_quote = true;
                quote = ch;
            }
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch)) && !in_quote) {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) args.push_back(current);
    return args;
}

static constexpr int kMaxUnrollLimit = 1000000;

static bool parseUnrollLimit(const std::string& text, int& value, std::string& error) {
    if (text.empty()) {
        error = "value is empty";
        return false;
    }
    unsigned long long parsed = 0;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            error = "value must be a positive decimal integer";
            return false;
        }
        parsed = parsed * 10ULL + static_cast<unsigned long long>(ch - '0');
        if (parsed > static_cast<unsigned long long>(kMaxUnrollLimit)) {
            error = "value exceeds maximum supported limit " + std::to_string(kMaxUnrollLimit);
            return false;
        }
    }
    if (parsed == 0) {
        error = "value must be greater than zero";
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

static int runMain(int argc, char* argv[]) {
    if (argc < 4) {
        printUsage(argv[0]);
        return 1;
    }

    std::string source_file;
    std::string top_function;
    std::string format = "listjson";
    std::string output_file;
    int unroll_limit = 1024;
    std::vector<std::string> clang_args;

    if (const char* env = std::getenv("PREDICATE_EXPAND_CLANG_ARGS")) {
        auto env_args = splitArgs(env);
        clang_args.insert(clang_args.end(), env_args.begin(), env_args.end());
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--input" || arg == "-i") && i + 1 < argc) {
            source_file = argv[++i];
        } else if (arg == "--top" && i + 1 < argc) {
            top_function = argv[++i];
        } else if (arg == "--format" && i + 1 < argc) {
            format = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--unroll-limit" && i + 1 < argc) {
            std::string error;
            std::string value = argv[++i];
            if (!parseUnrollLimit(value, unroll_limit, error)) {
                std::cerr << "Invalid --unroll-limit '" << value << "': " << error << "\n";
                return 1;
            }
        } else if (arg == "--clang-arg" && i + 1 < argc) {
            clang_args.push_back(argv[++i]);
        } else if (arg[0] != '-') {
            source_file = arg;
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (source_file.empty() || top_function.empty()) {
        printUsage(argv[0]);
        return 1;
    }
    if (!std::filesystem::exists(source_file)) {
        std::cerr << "Input file does not exist: " << source_file << "\n";
        return 1;
    }
    if (top_function.find_first_not_of(" \t\r\n") == std::string::npos) {
        std::cerr << "--top must not be empty\n";
        return 1;
    }
    if (format != "listjson") {
        std::cerr << "Unknown format: " << format << "\n";
        return 1;
    }

    // Step 1: Parse source with Clang
    auto build_result = pred::buildASTFromSource(source_file, top_function, clang_args);
    if (!build_result.error.empty()) {
        std::cerr << "Error: " << build_result.error << "\n";
        return 1;
    }
    if (!build_result.function.has_value()) {
        std::cerr << "Error: failed to extract function\n";
        return 1;
    }

    auto& func = build_result.function.value();

    // Step 2: Unroll loops
    pred::UnrollConfig unroll_cfg;
    unroll_cfg.max_iterations = unroll_limit;
    auto unroll_result = pred::unrollLoops(func.body, unroll_cfg);
    if (!unroll_result.error.empty()) {
        std::cerr << "Error during loop unrolling: " << unroll_result.error << "\n";
        return 1;
    }

    // Step 3: Subset/type checks and predicate-friendly normalization
    auto norm_result = pred::normalizeFunction(func, unroll_result.body);
    if (!norm_result.error.empty()) {
        std::cerr << "Error during subset/type checking: " << norm_result.error << "\n";
        return 1;
    }

    // Step 4: Build CFG from normalized body
    auto cfg = pred::buildCFG(norm_result.body);

    // Step 5: Build SSA
    auto ssa = pred::buildSSA(cfg, norm_result.ssa_seed_symbols);
    if (!ssa.error.empty()) {
        std::cerr << "Error during SSA construction: " << ssa.error << "\n";
        return 1;
    }

    // Step 6: Predication (if-conversion)
    auto pred_prog = pred::predicate(ssa);
    pred_prog.function_name = func.name;

    for (auto& [name, type] : norm_result.symbols) pred_prog.symbols[name] = type;
    pred_prog.param_directions = norm_result.param_directions;
    pred_prog.output_default_reasons = norm_result.output_default_reasons;
    pred_prog.output_paired_controls = norm_result.output_paired_controls;
    pred_prog.lookup_tables = norm_result.lookup_tables;
    pred_prog.outputs = norm_result.output_params;
    buildOutputExpressionMap(pred_prog);

    auto verify_result = pred::verifyPredicateProgram(pred_prog);
    if (!verify_result.ok) {
        std::cerr << "Error during Predicate IR verification: "
                  << verify_result.error << "\n";
        return 1;
    }

    // Step 7: Emit output
    std::string output;
    try {
        output = pred::emitListJson(pred_prog);
    } catch (const std::exception& ex) {
        std::cerr << "Error during listjson emission: " << ex.what() << "\n";
        return 1;
    }

    if (output_file.empty()) {
        std::cout << output;
    } else {
        std::filesystem::path out_path(output_file);
        if (out_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(out_path.parent_path(), ec);
            if (ec) {
                std::cerr << "Cannot create output directory: "
                          << out_path.parent_path().string() << ": "
                          << ec.message() << "\n";
                return 1;
            }
        }
        std::ofstream ofs(output_file);
        if (!ofs) {
            std::cerr << "Cannot open output file: " << output_file << "\n";
            return 1;
        }
        ofs << output;
    }

    return 0;
}

int main(int argc, char* argv[]) {
    try {
        return runMain(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Fatal error: unknown exception\n";
        return 1;
    }
}
