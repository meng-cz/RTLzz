#include "BackendIR.h"
#include "JsonParser.h"
#include "PredicateJsonLoader.h"

#include <exception>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

void printUsage(const char* program) {
    std::cerr << "Usage: " << program << " <predicate-json-file>\n";
}

std::string readFile(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open input file: " + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            printUsage(argv[0]);
            return 1;
        }

        rtlzz::json::Value root = rtlzz::json::parse(readFile(argv[1]));
        rtlzz::Program program = rtlzz::loadPredicateJson(root);
        rtlzz::printDebug(program, std::cout);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
