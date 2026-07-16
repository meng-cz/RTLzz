#include "rtlzz.hpp"

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
              << " <source.cpp> --top <function_name> [--format beir|rtl|listjson]"
              << " [--input source.cpp] [--vullib DIR] [--unroll-limit N]"
              << " [--beopt OPT ...] [--clang-arg ARG ...] [-o output_file]\n";
    std::cerr << "Default format is rtl. listjson is kept for API compatibility but is not supported by pipelinev2.\n";
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

static bool readSourceUnits(const std::string& path,
                            std::vector<std::string>& units,
                            std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "Cannot open input file: " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        units.push_back(line + "\n");
    }
    if (!in.eof()) {
        error = "Failed to read input file: " + path;
        return false;
    }
    return true;
}

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
    std::string format = "rtl";
    std::string output_file;
    std::string vullib_dir;
    int unroll_limit = 1024;
    std::vector<std::string> clang_args;
    std::vector<std::string> beopt_args;

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
        } else if ((arg == "--vullib" || arg == "--vullib-dir") && i + 1 < argc) {
            vullib_dir = argv[++i];
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
        } else if (arg == "--beopt" && i + 1 < argc) {
            beopt_args.push_back(argv[++i]);
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
    if (format != "listjson" && format != "beir" && format != "rtl") {
        std::cerr << "Unknown format: " << format << "\n";
        return 1;
    }

    rtlzz::CompileOptions options;
    std::string read_error;
    if (!readSourceUnits(source_file, options.source_codelines, read_error)) {
        std::cerr << read_error << "\n";
        return 1;
    }
    options.top_function = top_function;
    options.unroll_limit = unroll_limit;
    options.clang_args = std::move(clang_args);
    options.beopt_args = std::move(beopt_args);
    options.vullib_dir = vullib_dir;

    rtlzz::CompileResult result;
    if (format == "listjson") {
        result = rtlzz::compileToListJson(std::move(options));
    } else if (format == "beir") {
        result = rtlzz::compileToBeir(std::move(options));
    } else {
        result = rtlzz::compileToRtl(std::move(options));
    }
    if (!result.ok()) {
        std::cerr << "Error: " << result.error << "\n";
        return 1;
    }

    if (output_file.empty()) {
        for (const auto& line : result.output_codelines) std::cout << line;
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
        for (const auto& line : result.output_codelines) ofs << line;
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
