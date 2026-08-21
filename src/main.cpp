#include "chunk.h"
#include "scanner.h"
#include "vm.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(LOXPP_JVM_BACKEND) || defined(LOXPP_CLR_BACKEND)
#include "backend/abstract_stack.h"
#include "backend/chunk_decoder.h"
#include "compiler.h"
#include "memory_manager.h"

#include <filesystem>
#include <system_error>
#include <vector>
#endif

#ifdef LOXPP_JVM_BACKEND
#include "backend/jvm_emitter.h"
#endif

#ifdef LOXPP_CLR_BACKEND
#include "backend/clr_emitter.h"
#endif

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
        if (std::strncmp(kw, text, text_len) == 0) {
            return strdup(kw);
        }
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
        if (!line.empty()) {
            add_history(line.c_str());
        }
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

    if (result == InterpretResult::COMPILE_ERROR) {
        std::exit(65);
    }
    if (result == InterpretResult::RUNTIME_ERROR) {
        std::exit(70);
    }
}

#ifdef LOXPP_JVM_BACKEND
// Compiles `path` and writes one generated class per <outDir>/<name>.j:
// LoxMain (the top-level script) plus one LoxFn$<n> per function or method
// any chunk in the tree constructs. Does not assemble or run anything —
// tools/loxpp_jvm.sh chains jasmin and java on top, assembling every *.j
// file in the directory together. Exit codes mirror runFile's: 65 for a
// compile error, 70 for an opcode or CLOSURE shape the emitter does not
// lower (see jvm_emitter.h), 74 for a file-system failure.
static int runJvmTarget(const std::string& outDir, const std::string& path) {
    std::string source = readFile(path);

    MemoryManager mm;
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        return 65;
    }

    std::vector<jvm::EmittedClass> classes;
    try {
        DecodedFunction tree = decodeFunctionTree(script);
        StackAnalysisTree analysis = analyzeStackTree(tree);
        classes = jvm::emitProgram(tree, analysis, "LoxMain");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "loxpp --target jvm: %s\n", e.what());
        return 70;
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::fprintf(stderr,
                     "loxpp --target jvm: cannot create directory %s: %s\n",
                     outDir.c_str(), ec.message().c_str());
        return 74;
    }

    // Remove every stale *.j file first: tools/jvm_run.sh assembles every
    // *.j file it finds in outDir, so a class an earlier, larger run wrote
    // here would still reach the classpath even after this run's own source
    // no longer builds it.
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(outDir, ec)) {
        if (entry.path().extension() == ".j") {
            std::filesystem::remove(entry.path(), ec);
        }
    }

    for (const jvm::EmittedClass& cls : classes) {
        std::string outPath = outDir + "/" + cls.className + ".j";
        std::ofstream out(outPath, std::ios::binary);
        if (!out) {
            std::fprintf(stderr, "loxpp --target jvm: cannot write %s\n",
                         outPath.c_str());
            return 74;
        }
        out << cls.source;
    }
    return 0;
}
#endif

#ifdef LOXPP_CLR_BACKEND
// Compiles `path` and writes <outDir>/LoxMain.il — the top-level script,
// straight-line code only (clr_emitter.h). Does not assemble or run
// anything — tools/loxpp_clr.sh chains ilasm and dotnet on top. Exit codes
// mirror runJvmTarget's: 65 for a compile error, 70 for an opcode this node
// does not lower yet, 74 for a file-system failure.
static int runClrTarget(const std::string& outDir, const std::string& path) {
    std::string source = readFile(path);

    MemoryManager mm;
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        return 65;
    }

    std::string ilSource;
    try {
        DecodedFunction tree = decodeFunctionTree(script);
        FunctionStackAnalysis analysis = analyzeStack(tree);
        ilSource = clr::emitScript(tree, analysis, "LoxMain");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "loxpp --target clr: %s\n", e.what());
        return 70;
    }

    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        std::fprintf(stderr,
                     "loxpp --target clr: cannot create directory %s: %s\n",
                     outDir.c_str(), ec.message().c_str());
        return 74;
    }

    // Remove every stale *.il file first, same reason runJvmTarget clears
    // *.j: tools/clr_run.sh assembles every *.il file it finds in outDir.
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(outDir, ec)) {
        if (entry.path().extension() == ".il") {
            std::filesystem::remove(entry.path(), ec);
        }
    }

    std::string outPath = outDir + "/LoxMain.il";
    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "loxpp --target clr: cannot write %s\n",
                     outPath.c_str());
        return 74;
    }
    out << ilSource;
    return 0;
}
#endif

// The set of `--target` values this build recognizes, in usage-message form.
// Must list only backends this translation unit actually compiled in, so the
// message never offers a target the two #ifdef dispatch arms below refuse.
#if defined(LOXPP_JVM_BACKEND) && defined(LOXPP_CLR_BACKEND)
#define LOXPP_TARGET_USAGE_LIST "{jvm,clr}"
#elif defined(LOXPP_JVM_BACKEND)
#define LOXPP_TARGET_USAGE_LIST "{jvm}"
#elif defined(LOXPP_CLR_BACKEND)
#define LOXPP_TARGET_USAGE_LIST "{clr}"
#endif

int main(int argc, const char* argv[]) {
#if defined(LOXPP_JVM_BACKEND) || defined(LOXPP_CLR_BACKEND)
    // loxpp --target {jvm,clr} --out-dir <dir> program.lox — compiles only,
    // never runs the program. Only intercepted when the first argument is
    // exactly "--target", so plain `loxpp [path]` keeps its existing
    // behaviour. Which `target` values are recognized here depends on which
    // backends this build was configured with.
    if (argc >= 2 && std::string(argv[1]) == "--target") {
        std::string target;
        std::string outDir;
        std::string scriptPath;
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--target" && i + 1 < argc) {
                target = argv[++i];
            } else if (arg == "--out-dir" && i + 1 < argc) {
                outDir = argv[++i];
            } else {
                scriptPath = arg;
            }
        }
        if (outDir.empty() || scriptPath.empty()) {
            std::fprintf(stderr,
                         "Usage: loxpp --target " LOXPP_TARGET_USAGE_LIST
                         " --out-dir <dir> program.lox\n");
            return 64;
        }
#ifdef LOXPP_JVM_BACKEND
        if (target == "jvm") {
            return runJvmTarget(outDir, scriptPath);
        }
#endif
#ifdef LOXPP_CLR_BACKEND
        if (target == "clr") {
            return runClrTarget(outDir, scriptPath);
        }
#endif
        std::fprintf(stderr, "Usage: loxpp --target " LOXPP_TARGET_USAGE_LIST
                             " --out-dir <dir> program.lox\n");
        return 64;
    }
#endif

    VM vm;

    if (argc == 1) {
        repl(vm);
        return 0;
    }

    std::vector<std::string> progArgs;
    for (int i = 2; i < argc; i++) {
        progArgs.emplace_back(argv[i]);
    }
    vm.setArgs(std::move(progArgs));
    runFile(vm, argv[1]);

    return 0;
}
