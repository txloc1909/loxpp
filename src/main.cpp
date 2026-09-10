#include "analyze.h"
#include "chunk.h"
#include "diagnostic.h"
#include "json_escape.h"
#include "scanner.h"
#include "vm.h"
#include "loxpp_version.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <optional>
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

#include <isocline.h>
#include <cstdlib>
#include <regex>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

static void lox_word_completer(ic_completion_env_t* cenv, const char* word) {
    ic_add_completions(cenv, word, (const char**)lox_keywords());
}

static void lox_completer(ic_completion_env_t* cenv, const char* prefix) {
    ic_complete_word(cenv, prefix, lox_word_completer, nullptr);
}

static void repl(VM& vm) {
    std::string history_path = xdg_history_path();
    ic_set_history(history_path.c_str(), 1000);
    ic_set_default_completer(lox_completer, nullptr);

    for (;;) {
        char* raw = ic_readline("> ");
        if (!raw) {
            std::cout << std::endl;
            break;
        }
        std::string line(raw);
        free(raw);
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

static const char* severityWord(Severity severity) {
    switch (severity) {
    case Severity::Warning:
        return "warning";
    case Severity::Info:
        return "info";
    case Severity::Error:
    default:
        return "error";
    }
}

// loxpp --check [--format text|json] <file>: report the compiler's static
// errors without running the program. Exit 0 when clean, 1 when any error,
// 74 when the file cannot be read (readFile), 64 on a usage error.
static int runCheck(int argc, const char* argv[]) {
    std::string format = "text";
    std::string path;
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--format") {
            if (i + 1 >= argc) {
                std::fprintf(
                    stderr,
                    "Usage: loxpp --check [--format text|json] <file>\n");
                return 64;
            }
            format = argv[++i];
        } else if (path.empty()) {
            path = arg;
        } else {
            std::fprintf(stderr,
                         "Usage: loxpp --check [--format text|json] <file>\n");
            return 64;
        }
    }
    if (path.empty() || (format != "text" && format != "json")) {
        std::fprintf(stderr,
                     "Usage: loxpp --check [--format text|json] <file>\n");
        return 64;
    }

    std::string source = readFile(path);
    std::vector<Diagnostic> diagnostics = analyze(source);
    LineIndex index(source);

    if (format == "json") {
        std::string out = "[";
        for (std::size_t i = 0; i < diagnostics.size(); i++) {
            const Diagnostic& d = diagnostics[i];
            auto [endLine, endColumn] = index.locate(d.offset + d.length);
            if (i > 0) {
                out += ',';
            }
            out += "{\"line\":" + std::to_string(d.line - 1);
            out += ",\"character\":" + std::to_string(d.column - 1);
            out += ",\"endLine\":" + std::to_string(endLine - 1);
            out += ",\"endCharacter\":" + std::to_string(endColumn - 1);
            out += ",\"severity\":";
            appendJsonString(out, severityWord(d.severity));
            out += ",\"message\":";
            appendJsonString(out, d.message);
            out += '}';
        }
        out += "]";
        std::printf("%s\n", out.c_str());
    } else {
        for (const Diagnostic& d : diagnostics) {
            std::printf("%s:%zu:%zu: %s: %s\n", path.c_str(), d.line, d.column,
                        severityWord(d.severity), d.message.c_str());
        }
    }

    bool anyError = std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [](const Diagnostic& d) { return d.severity == Severity::Error; });
    return anyError ? 1 : 0;
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
// the whole reachable program (clr_emitter.h): the script's own class plus
// one generated class per function or method. Does not assemble or run
// anything — tools/loxpp_clr.sh chains ilasm and dotnet on top. Exit codes
// mirror runJvmTarget's: 65 for a compile error, 70 for an opcode or
// CLOSURE shape the emitter does not lower (see clr_emitter.h), 74 for a
// file-system failure.
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
        StackAnalysisTree analysis = analyzeStackTree(tree);
        ilSource = clr::emitProgram(tree, analysis, "LoxMain");
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

// --- loxpp upgrade -------------------------------------------------------
//
// `loxpp upgrade` is a thin front-end over install.sh. It never downloads
// or verifies the binary itself: it fetches the canonical install.sh and
// runs it with `sh`. All TLS, checksum, and signature logic stays in the
// script, so the binary carries no CA bundle and there is one copy of the
// verify path.

namespace {

// The installer that `loxpp upgrade` runs. `raw ... /main/install.sh` is
// always the current script, so a fix to the verify logic reaches every
// installed binary on its next upgrade. The URL is fixed with no override:
// `loxpp upgrade` runs code from this path with the user's rights, so a
// changed environment must not be able to redirect it to another script.
constexpr const char* kInstallShUrl =
    "https://raw.githubusercontent.com/txloc1909/loxpp/main/install.sh";

// The SHA256SUMS of the newest non-pre-release. github.com, not the API, so
// there is no rate limit. install.sh reads the same file the same way.
constexpr const char* kLatestSumsUrl =
    "https://github.com/txloc1909/loxpp/releases/latest/download/SHA256SUMS";

bool hasCommand(const char* name) {
    std::string probe = "command -v ";
    probe += name;
    probe += " >/dev/null 2>&1";
    return std::system(probe.c_str()) == 0;
}

std::string singleQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

// Run `curl`/`wget` through the shell and return the response body, or
// nullopt when neither tool is present or the transfer fails.
std::optional<std::string> httpGet(const std::string& url) {
    std::string cmd;
    if (hasCommand("curl")) {
        cmd = "curl -fsSL " + singleQuote(url);
    } else if (hasCommand("wget")) {
        cmd = "wget -q -O - " + singleQuote(url);
    } else {
        return std::nullopt;
    }
    // The caller prints its own message on failure; keep the transport
    // tool's own stderr out of the way.
    cmd += " 2>/dev/null";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return std::nullopt;
    }
    std::string body;
    std::string chunk(4096, '\0');
    size_t got = 0;
    while ((got = std::fread(chunk.data(), 1, chunk.size(), pipe)) > 0) {
        body.append(chunk.data(), got);
    }
    int status = pclose(pipe);
    if (status != 0) {
        return std::nullopt;
    }
    return body;
}

// Pull the version out of an asset name, e.g.
// "loxpp-0.1.0-x86_64-linux.tar.gz" -> "0.1.0". Non-greedy so the capture
// stops at the first "-x86_64-linux".
std::optional<std::string> versionFromSums(const std::string& sums) {
    static const std::regex asset(R"(loxpp-(.+?)-x86_64-linux\.tar\.gz)");
    std::smatch match;
    if (std::regex_search(sums, match, asset)) {
        return match[1].str();
    }
    return std::nullopt;
}

struct SemVer {
    long major = 0;
    long minor = 0;
    long patch = 0;
    std::string pre;
    bool parsed = false;
};

SemVer parseSemVer(const std::string& text) {
    static const std::regex form(R"(^(\d+)\.(\d+)\.(\d+)(?:-(.+))?$)");
    std::smatch match;
    SemVer out;
    if (std::regex_match(text, match, form)) {
        out.major = std::stol(match[1].str());
        out.minor = std::stol(match[2].str());
        out.patch = std::stol(match[3].str());
        out.pre = match[4].matched ? match[4].str() : "";
        out.parsed = true;
    }
    return out;
}

// Returns <0 when `a` is older, 0 when equal, >0 when `a` is newer. A
// pre-release suffix (0.1.0-rc1) sorts before the same version with none
// (0.1.0), per semver. Unparseable input falls back to a string compare.
int compareVersions(const std::string& a, const std::string& b) {
    SemVer va = parseSemVer(a);
    SemVer vb = parseSemVer(b);
    if (!va.parsed || !vb.parsed) {
        return a.compare(b);
    }
    if (va.major != vb.major) {
        return va.major < vb.major ? -1 : 1;
    }
    if (va.minor != vb.minor) {
        return va.minor < vb.minor ? -1 : 1;
    }
    if (va.patch != vb.patch) {
        return va.patch < vb.patch ? -1 : 1;
    }
    if (va.pre == vb.pre) {
        return 0;
    }
    if (va.pre.empty()) {
        return 1;
    }
    if (vb.pre.empty()) {
        return -1;
    }
    return va.pre < vb.pre ? -1 : 1;
}

// Resolve the real path of this executable through /proc/self/exe (which
// dereferences a ~/.local/bin/loxpp symlink) and return its directory.
std::optional<std::string> exeDir() {
    std::string buffer(4096, '\0');
    ssize_t len = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (len <= 0 || static_cast<size_t>(len) >= buffer.size()) {
        return std::nullopt;
    }
    buffer.resize(static_cast<size_t>(len));
    size_t slash = buffer.find_last_of('/');
    if (slash == std::string::npos) {
        return std::string(".");
    }
    if (slash == 0) {
        return std::string("/");
    }
    return buffer.substr(0, slash);
}

std::optional<std::string> latestReleaseVersion() {
    std::optional<std::string> sums = httpGet(kLatestSumsUrl);
    if (!sums) {
        return std::nullopt;
    }
    return versionFromSums(*sums);
}

// `loxpp upgrade --check`: compare the running version to the newest
// release. Changes nothing.
int runUpgradeCheck() {
    std::optional<std::string> latest = latestReleaseVersion();
    if (!latest) {
        std::fprintf(stderr,
                     "loxpp upgrade --check: cannot read the latest release. "
                     "Need curl or wget and a network connection.\n");
        return 74;
    }
    const std::string current = LOXPP_VERSION;
    if (compareVersions(current, *latest) >= 0) {
        std::printf("loxpp is up to date (%s)\n", current.c_str());
    } else {
        std::printf("loxpp %s is available (you have %s); run: loxpp upgrade\n",
                    latest->c_str(), current.c_str());
    }
    return 0;
}

bool isSafeVersion(const std::string& v) {
    if (v.empty()) {
        return false;
    }
    return std::all_of(v.begin(), v.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' ||
               c == '-' || c == '+';
    });
}

// `loxpp upgrade [--version X.Y.Z]`: hand off to install.sh.
int runUpgradeApply(const std::string& version) {
    std::optional<std::string> dir = exeDir();
    if (!dir) {
        std::fprintf(stderr, "loxpp upgrade: cannot resolve /proc/self/exe.\n");
        return 74;
    }

    // The installer writes a temp file in this directory and renames it
    // over the running binary. Fail here, before any download, when the
    // directory is not writable.
    if (::access(dir->c_str(), W_OK) != 0) {
        std::fprintf(stderr,
                     "loxpp upgrade: %s is not writable.\n"
                     "Set LOXPP_INSTALL_DIR to a writable directory, or run "
                     "with sudo:\n"
                     "  sudo loxpp upgrade\n",
                     dir->c_str());
        return 74;
    }

    if (!version.empty() && !isSafeVersion(version)) {
        std::fprintf(stderr, "loxpp upgrade: invalid --version value.\n");
        return 64;
    }

    // Without an explicit --version, skip the work when already current.
    if (version.empty()) {
        std::optional<std::string> latest = latestReleaseVersion();
        if (latest &&
            compareVersions(std::string(LOXPP_VERSION), *latest) >= 0) {
            std::printf("loxpp is up to date (%s)\n", LOXPP_VERSION);
            return 0;
        }
    }

    const bool haveCurl = hasCommand("curl");
    const bool haveWget = hasCommand("wget");
    if ((!haveCurl && !haveWget) || !hasCommand("sh")) {
        std::fprintf(
            stderr,
            "loxpp upgrade: need (curl or wget) and sh. Install manually:\n"
            "  curl -fsSL %s | sh\n",
            kInstallShUrl);
        return 74;
    }

    // Download the installer to a file and check the transfer first, then
    // run it. A piped `curl ... | sh` hides a failed fetch: the pipeline
    // exit status is `sh`'s, and `sh` on empty input exits 0, so a 404 or a
    // dropped network would look like a clean upgrade that changed nothing.
    std::optional<std::string> script = httpGet(kInstallShUrl);
    if (!script || script->empty()) {
        std::fprintf(stderr,
                     "loxpp upgrade: could not download the installer from\n"
                     "  %s\n"
                     "Check your network connection, then try again.\n",
                     kInstallShUrl);
        return 74;
    }

    const char* tmpDir = std::getenv("TMPDIR");
    std::string scriptPath =
        (tmpDir != nullptr && tmpDir[0] != '\0' ? std::string(tmpDir)
                                                : std::string("/tmp")) +
        "/loxpp-upgrade.XXXXXX";
    std::vector<char> pathBuf(scriptPath.begin(), scriptPath.end());
    pathBuf.push_back('\0');
    int fd = ::mkstemp(pathBuf.data());
    if (fd < 0) {
        std::fprintf(
            stderr,
            "loxpp upgrade: cannot create a temp file for the installer.\n");
        return 74;
    }
    scriptPath = pathBuf.data();

    bool writeOk = true;
    size_t written = 0;
    while (written < script->size()) {
        ssize_t n =
            ::write(fd, script->data() + written, script->size() - written);
        if (n <= 0) {
            writeOk = false;
            break;
        }
        written += static_cast<size_t>(n);
    }
    ::close(fd);
    if (!writeOk) {
        ::unlink(scriptPath.c_str());
        std::fprintf(stderr,
                     "loxpp upgrade: cannot write the installer to disk.\n");
        return 74;
    }

    std::string cmd = "sh " + singleQuote(scriptPath) + " --";
    cmd += " --bin-dir " + singleQuote(*dir);
    if (!version.empty()) {
        cmd += " --version " + singleQuote(version);
    }

    int status = std::system(cmd.c_str());
    ::unlink(scriptPath.c_str());
    if (status == -1) {
        std::fprintf(stderr, "loxpp upgrade: could not start the installer.\n");
        return 74;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 70;
}

int runUpgrade(int argc, const char* const* argv) {
    std::string version;
    bool check = false;
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--check") {
            check = true;
        } else if (arg == "--version" && i + 1 < argc) {
            version = argv[++i];
        } else {
            std::fprintf(stderr,
                         "Usage: loxpp upgrade [--check] [--version X.Y.Z]\n");
            return 64;
        }
    }
    if (check) {
        return runUpgradeCheck();
    }
    return runUpgradeApply(version);
}

} // namespace

// Print version and bundled library info, then exit.
static void printVersion() {
    std::printf("loxpp %s\n", LOXPP_VERSION);
    std::printf("Bundled: isocline %s (© Daan Leijen, MIT)\n",
                LOXPP_ISOCLINE_VERSION);
}

// Print usage help with exit codes, then exit.
static void printHelp() {
    std::printf("Usage:\n");
    std::printf("  loxpp [path]                           Run a script or "
                "enter REPL\n");
    std::printf("  loxpp --version                        Print version "
                "and dependencies\n");
    std::printf("  loxpp --help                           Print this help "
                "message\n");
    std::printf("  loxpp --check [--format text|json] <file>\n");
    std::printf("                                         Check syntax "
                "without running\n");
    std::printf("  loxpp upgrade [--check] [--version X.Y.Z]\n");
    std::printf("                                         Update loxpp to "
                "the latest release\n");
    std::printf("\n");
    std::printf("Exit codes:\n");
    std::printf("  0   Success\n");
    std::printf("  1   Compile error when checking (--check only)\n");
    std::printf("  64  Usage error (bad arguments or flags)\n");
    std::printf("  65  Compile error when running a script\n");
    std::printf("  70  Runtime error when running a script\n");
    std::printf("  74  File system error (file not found, cannot read, "
                "etc.)\n");
}

// Dispatch early info flags (--version, --help, --check, --target) that
// exit before VM initialization. Returns std::nullopt if the caller should
// proceed to normal execution; returns an exit code if a flag was handled.
// Reduces cognitive complexity of main() by extracting the nested conditions.
static std::optional<int> dispatchEarlyFlags(int argc, const char* argv[]) {
    if (argc < 2) {
        return std::nullopt;
    }

    std::string flag = argv[1];

    if (flag == "--version") {
        printVersion();
        return 0;
    }

    if (flag == "--help") {
        printHelp();
        return 0;
    }

    if (flag == "--check") {
        return runCheck(argc, argv);
    }

    if (flag == "upgrade") {
        return runUpgrade(argc, argv);
    }

#if defined(LOXPP_JVM_BACKEND) || defined(LOXPP_CLR_BACKEND)
    if (flag == "--target") {
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

    return std::nullopt;
}

int main(int argc, const char* argv[]) {
    // Handle early exit flags: --version, --help, --check, --target.
    auto exitCode = dispatchEarlyFlags(argc, argv);
    if (exitCode.has_value()) {
        return exitCode.value();
    }

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
