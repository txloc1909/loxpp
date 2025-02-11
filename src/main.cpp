#include "chunk.h"
#include "vm.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

static void repl(VM& vm) {
    std::string line;
    for (;;) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
            std::cout << std::endl;
            break;
        }
        vm.interpret(line);
    }
}

static std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::fprintf(stderr, "Could not open file \"%s\".", path.c_str());
        std::exit(74);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static void runFile(VM& vm, const std::string& path) {
    std::string source = readFile(path);
    InterpretResult result = vm.interpret(source);

    if (result == InterpretResult::COMPILE_ERROR)
        std::exit(65);
    if (result == InterpretResult::RUNTIME_ERROR)
        std::exit(70);
}

int main(int argc, const char* argv[]) {
    VM vm;

    if (argc == 1) {
        repl(vm);
    } else if (argc == 2) {
        runFile(vm, argv[1]);
    } else {
        std::fprintf(stderr, "Usage: loxpp [path]\n");
        std::exit(64);
    }

    return 0;
}
