#include "lsp/stdlib_docs.h"

#include <array>

#include "tooling/stdlib_names.h"

namespace loxpp::lsp {

namespace {

// Globals: src/stdlib/globals.cpp, file_api.cpp, os_api.cpp, reflect_api.cpp,
// math_module.cpp. Descriptions condensed from spec/05-stdlib.md.
constexpr std::array<StdlibEntry, 27> kGlobals = {{
    {"clock", "clock() -> Number", 0,
     "Elapsed processor time in seconds. Use for measuring durations."},
    {"input", "input() -> String | Nil", 0,
     "Reads one line from standard input without the newline; nil at EOF."},
    {"ord", "ord(value) -> Number", 1,
     "The byte value 0-255 of a one-byte String."},
    {"chr", "chr(value) -> String", 1,
     "The one-byte String for an integer byte value 0-255."},
    {"open", "open(path, mode) -> File", 2,
     "Opens a file in mode r, w, a, or r+. Returns a File."},
    {"args", "args() -> List[String]", 0,
     "The command-line arguments after the program file name."},
    {"env", "env(name) -> String | Nil", 1,
     "The value of environment variable `name`, or nil when it is not set."},
    {"exit", "exit(code)", 1,
     "Ends the program at once with integer exit code `code`."},
    {"time", "time() -> Number", 0,
     "Seconds since the Unix epoch. Use for calendar time, not durations."},
    {"sleep", "sleep(seconds) -> Nil", 1,
     "Suspends the program for at least `seconds` seconds."},
    {"exists", "exists(path) -> Bool", 1,
     "True when a file or directory exists at `path`."},
    {"is_dir", "is_dir(path) -> Bool", 1,
     "True when `path` exists and is a directory."},
    {"is_file", "is_file(path) -> Bool", 1,
     "True when `path` exists and is a regular file."},
    {"stat", "stat(path) -> Map | Nil", 1,
     "A map of file metadata (is_dir, is_file, size, mtime), or nil."},
    {"connect", "connect(host, port) -> Socket", 2,
     "Opens a TCP connection and returns a connected Socket."},
    {"listen", "listen(host, port) -> Server", 2,
     "Binds a listening TCP socket; port 0 picks a free port."},
    {"spawn", "spawn(program, args) -> Process", 2,
     "Starts a child process with pipes; args is a List of Strings."},
    {"run", "run(program, args) -> Map", 2,
     "Runs a child to completion; returns status, stdout, and stderr."},
    {"type", "type(value) -> String", 1,
     "The language-level type name of `value`, such as \"Number\"."},
    {"fields", "fields(instance) -> List[String]", 1,
     "The names of every field set on `instance`. Methods are not fields."},
    {"methods", "methods(class) -> List[String]", 1,
     "The names of every method `class` responds to, own and inherited."},
    {"getField", "getField(instance, name) -> Any", 2,
     "The value of field `name` on `instance`, or nil when it is absent."},
    {"setField", "setField(instance, name, value) -> Any", 3,
     "Sets field `name` on `instance` to `value`; returns `value`."},
    {"hasField", "hasField(instance, name) -> Boolean", 2,
     "True when `instance` has a field named `name`."},
    {"callMethod", "callMethod(instance, name, ...args) -> Any", kArityVariadic,
     "Calls native method or callable field `name` on `instance` with `args`."},
    {"math", "math", kArityConstant,
     "Global object of numeric functions and constants, reached with `.`."},
    {"coroutine", "coroutine", kArityConstant,
     "Global object whose `create(fn)` makes a suspended coroutine, reached "
     "with `.`."},
}};

// `math.<name>`: src/math.cpp kMathFunctions and kMathConstants.
constexpr std::array<StdlibEntry, 25> kMath = {{
    {"abs", "math.abs(x) -> Number", 1, "Absolute value of `x`."},
    {"ceil", "math.ceil(x) -> Number", 1,
     "Smallest integer value not less than `x`."},
    {"floor", "math.floor(x) -> Number", 1,
     "Largest integer value not greater than `x`."},
    {"round", "math.round(x) -> Number", 1,
     "Nearest integer; a halfway value rounds away from zero."},
    {"sqrt", "math.sqrt(x) -> Number", 1,
     "Square root of `x`. A negative `x` gives nan."},
    {"cbrt", "math.cbrt(x) -> Number", 1, "Cube root of `x`."},
    {"exp", "math.exp(x) -> Number", 1, "e raised to the power `x`."},
    {"log", "math.log(x) -> Number", 1, "Natural logarithm (base e) of `x`."},
    {"log2", "math.log2(x) -> Number", 1, "Base-2 logarithm of `x`."},
    {"log10", "math.log10(x) -> Number", 1, "Base-10 logarithm of `x`."},
    {"sin", "math.sin(x) -> Number", 1, "Sine of `x` radians."},
    {"cos", "math.cos(x) -> Number", 1, "Cosine of `x` radians."},
    {"tan", "math.tan(x) -> Number", 1, "Tangent of `x` radians."},
    {"asin", "math.asin(x) -> Number", 1, "Arc sine of `x`, in radians."},
    {"acos", "math.acos(x) -> Number", 1, "Arc cosine of `x`, in radians."},
    {"atan", "math.atan(x) -> Number", 1, "Arc tangent of `x`, in radians."},
    {"pow", "math.pow(x, y) -> Number", 2, "`x` raised to the power `y`."},
    {"atan2", "math.atan2(y, x) -> Number", 2,
     "Arc tangent of `y / x`, using the signs of both to pick the quadrant."},
    {"hypot", "math.hypot(x, y) -> Number", 2,
     "Length of the hypotenuse: sqrt(x*x + y*y)."},
    {"min", "math.min(x, y) -> Number", 2,
     "The smaller of `x` and `y`. If one is nan, the other is returned."},
    {"max", "math.max(x, y) -> Number", 2,
     "The larger of `x` and `y`. If one is nan, the other is returned."},
    {"pi", "math.pi", kArityConstant,
     "Ratio of a circle's circumference to its diameter."},
    {"e", "math.e", kArityConstant, "Base of the natural logarithm."},
    {"inf", "math.inf", kArityConstant, "Positive infinity."},
    {"nan", "math.nan", kArityConstant, "An IEEE 754 quiet NaN."},
}};

// Built-in methods on Map values (src/stdlib/map_api.cpp), File values
// (src/stdlib/file_api.cpp), and the Socket/Server/Process method names that
// do not already appear here (src/stdlib/net_api.cpp, process_api.cpp).
// Descriptions from spec/05-stdlib.md and spec/03-types.md.
constexpr std::array<StdlibEntry, 25> kMethods = {{
    {"has", "map.has(key) -> Boolean", 1, "True when the map contains `key`."},
    {"del", "map.del(key) -> Boolean", 1,
     "Removes `key` from the map; true when it was present."},
    {"keys", "map.keys() -> List", 0, "A list of the map keys."},
    {"values", "map.values() -> List", 0, "A list of the map values."},
    {"entries", "map.entries() -> List", 0,
     "A list of [key, value] pairs, one per entry."},
    {"read", "file.read() -> String", 0,
     "Reads the rest of the file as one string."},
    {"readline", "file.readline() -> String | Nil", 0,
     "Reads the next line without its newline; nil at end of file."},
    {"readlines", "file.readlines() -> List[String]", 0,
     "Reads the rest of the file as a list of lines."},
    {"read_bytes", "socket.read_bytes(n) -> String", 1,
     "Reads up to `n` bytes, blocking until `n` arrive or the peer closes."},
    {"write", "file.write(text) -> Nil", 1,
     "Writes `text` to the file. No newline is added."},
    {"writeline", "file.writeline(text) -> Nil", 1,
     "Writes `text` followed by one newline."},
    {"close", "file.close() -> Nil", 0,
     "Closes the file. A second call does nothing."},
    {"accept", "server.accept() -> Socket", 0,
     "Waits for the next connection and returns it as a Socket."},
    {"port", "server.port() -> Number", 0,
     "The local port the Server is bound to."},
    {"close_write", "socket.close_write() -> Nil", 0,
     "Half-closes a Socket, sending EOF while reads stay open."},
    {"close_stdin", "process.close_stdin() -> Nil", 0,
     "Closes a child process's standard input."},
    {"read_err", "process.read_err() -> String", 0,
     "Reads the child's standard error as one string."},
    {"err_read_bytes", "process.err_read_bytes(n) -> String", 1,
     "Reads up to `n` bytes of the child's standard error."},
    {"err_readline", "process.err_readline() -> String | Nil", 0,
     "Reads the child's next standard-error line; nil at EOF."},
    {"err_readlines", "process.err_readlines() -> List[String]", 0,
     "Reads the child's remaining standard-error lines."},
    {"wait", "process.wait() -> Number", 0,
     "Waits for the child to exit; returns its status and reaps it."},
    {"kill", "process.kill() -> Nil", 0,
     "Terminates the child at once; call wait() to reap it."},
    {"pid", "process.pid() -> Number", 0, "The child's process identifier."},
    {"resume", "co.resume(...args) -> Any", kArityVariadic,
     "Runs the coroutine until it yields or returns; returns that value."},
    {"status", "co.status() -> String", 0,
     R"(One of "suspended", "running", "normal", or "dead".)"},
}};

// `coroutine.<name>`: src/stdlib/coroutine_api.cpp.
constexpr std::array<StdlibEntry, 1> kCoroutine = {{
    {"create", "coroutine.create(fn) -> Coroutine", 1,
     "Makes a new suspended coroutine that runs `fn` when first resumed."},
}};

const StdlibEntry* find(const auto& table, std::string_view name) {
    for (const StdlibEntry& e : table) {
        if (e.name == name) {
            return &e;
        }
    }
    return nullptr;
}

} // namespace

const StdlibEntry* stdlibGlobalDoc(std::string_view name) {
    return find(kGlobals, name);
}

const StdlibEntry* mathMemberDoc(std::string_view name) {
    return find(kMath, name);
}

const StdlibEntry* coroutineMemberDoc(std::string_view name) {
    return find(kCoroutine, name);
}

const StdlibEntry* methodDoc(std::string_view name) {
    return find(kMethods, name);
}

const std::vector<StdlibEntry>& allGlobalDocs() {
    static const std::vector<StdlibEntry> v(kGlobals.begin(), kGlobals.end());
    return v;
}

const std::vector<StdlibEntry>& allMathMemberDocs() {
    static const std::vector<StdlibEntry> v(kMath.begin(), kMath.end());
    return v;
}

const std::vector<StdlibEntry>& allCoroutineMemberDocs() {
    static const std::vector<StdlibEntry> v(kCoroutine.begin(),
                                            kCoroutine.end());
    return v;
}

const std::vector<StdlibEntry>& allMethodDocs() {
    static const std::vector<StdlibEntry> v(kMethods.begin(), kMethods.end());
    return v;
}

std::string renderHover(const StdlibEntry& entry) {
    std::string out = "```lox\n";
    out += entry.signature;
    out += "\n```\n";
    if (entry.arity >= 0) {
        out += "**Arity:** " + std::to_string(entry.arity) + "  \n";
    } else if (entry.arity == kArityVariadic) {
        out += "**Arity:** variadic  \n";
    }
    out += std::string(entry.description);
    return out;
}

std::vector<std::string> stdlibDocsMissingNames() {
    std::vector<std::string> missing;
    for (std::string_view name : tooling::stdlibGlobals()) {
        if (stdlibGlobalDoc(name) == nullptr) {
            missing.emplace_back(name);
        }
    }
    for (std::string_view name : tooling::mathMembers()) {
        if (mathMemberDoc(name) == nullptr) {
            missing.emplace_back(name);
        }
    }
    return missing;
}

} // namespace loxpp::lsp
