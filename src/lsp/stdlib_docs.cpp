#include "lsp/stdlib_docs.h"

#include <array>

#include "tooling/stdlib_names.h"

namespace loxpp::lsp {

namespace {

// Globals: src/stdlib/globals.cpp, file_api.cpp, os_api.cpp, reflect_api.cpp,
// math_module.cpp. Descriptions condensed from spec/05-stdlib.md.
constexpr std::array<StdlibEntry, 22> kGlobals = {{
    {"clock", "clock() -> Number", 0,
     "Elapsed processor time in seconds. Use for measuring durations."},
    {"input", "input() -> String | Nil", 0,
     "Reads one line from standard input without the newline; nil at EOF."},
    {"str", "str(value) -> String", 1,
     "Converts any value to its canonical string form, as `print` shows it."},
    {"len", "len(seq) -> Number", 1,
     "Number of elements in a list, bytes in a string, or pairs in a map."},
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

// Built-in methods on Map values (src/stdlib/map_api.cpp) and File values
// (src/stdlib/file_api.cpp). Descriptions from spec/05-stdlib.md and
// spec/03-types.md.
constexpr std::array<StdlibEntry, 11> kMethods = {{
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
    {"write", "file.write(text) -> Nil", 1,
     "Writes `text` to the file. No newline is added."},
    {"writeline", "file.writeline(text) -> Nil", 1,
     "Writes `text` followed by one newline."},
    {"close", "file.close() -> Nil", 0,
     "Closes the file. A second call does nothing."},
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
