#include "chunk.h"
#include "scanner.h"
#include "vm.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

#ifdef LOXPP_USE_READLINE
#include <readline/readline.h>
#include <readline/history.h>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

static char* keyword_generator(const char* text, int state) {
    static int list_index;
    static std::size_t text_len;

    if (!state) {
        list_index = 0;
        text_len = std::strlen(text);
    }

    const char* const* keywords = lox_keywords();
    while (keywords[list_index]) {
        const char* kw = keywords[list_index++];
        if (std::strncmp(kw, text, text_len) == 0)
            return strdup(kw);
    }
    return nullptr;
}

static char** lox_completion(const char* text, int /*start*/, int /*end*/) {
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, keyword_generator);
}

static std::string xdg_history_path() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    std::string dir =
        xdg ? std::string(xdg)
            : (home != nullptr ? std::string(home) + "/.cache" : "/tmp");
    dir += "/loxpp";
    mkdir(dir.c_str(), 0755);
    return dir + "/history";
}
#endif

static void repl(VM& vm) {
#ifdef LOXPP_USE_READLINE
    std::string history_path = xdg_history_path();
    using_history();
    stifle_history(1000);
    read_history(history_path.c_str());

    rl_attempted_completion_function = lox_completion;

    for (;;) {
        char* raw = readline("> ");
        if (!raw) {
            std::cout << std::endl;
            break;
        }
        std::string line(raw);
        free(raw);
        if (!line.empty())
            add_history(line.c_str());
        vm.interpret(line);
    }

    write_history(history_path.c_str());
#else
    std::string line;
    for (;;) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
            std::cout << std::endl;
            break;
        }
        vm.interpret(line);
    }
#endif
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
