#include "tooling/stdlib_names.h"

#include <algorithm>
#include <array>

namespace loxpp::tooling {

namespace {

// From src/stdlib/globals.cpp (clock input ord chr), src/stdlib/file_api.cpp
// (open), src/stdlib/os_api.cpp (args env exit time sleep exists is_dir
// is_file stat), src/stdlib/net_api.cpp (connect listen), src/stdlib/
// process_api.cpp (spawn run), src/stdlib/reflect_api.cpp (type fields
// methods getField setField hasField callMethod), src/stdlib/math_module.cpp
// (math), src/stdlib/coroutine_api.cpp (coroutine). `print`, `len`, and `str`
// are statement/operator keywords, not globals, so they are not listed.
constexpr std::array<std::string_view, 27> kGlobals = {
    "clock",      "input",  "ord",       "chr",      "open",     "args",
    "env",        "exit",   "time",      "sleep",    "exists",   "is_dir",
    "is_file",    "stat",   "connect",   "listen",   "spawn",    "run",
    "type",       "fields", "methods",   "getField", "setField", "hasField",
    "callMethod", "math",   "coroutine",
};

// From src/math.cpp: kMathFunctions and kMathConstants.
constexpr std::array<std::string_view, 25> kMath = {
    "abs",   "ceil", "floor", "round", "sqrt", "cbrt", "exp",  "log", "log2",
    "log10", "sin",  "cos",   "tan",   "asin", "acos", "atan", "pow", "atan2",
    "hypot", "min",  "max",   "pi",    "e",    "inf",  "nan",
};

} // namespace

std::span<const std::string_view> stdlibGlobals() {
    return {kGlobals.data(), kGlobals.size()};
}

std::span<const std::string_view> mathMembers() {
    return {kMath.data(), kMath.size()};
}

bool isStdlibGlobal(std::string_view name) {
    return std::ranges::find(kGlobals, name) != kGlobals.end();
}

bool isMathMember(std::string_view name) {
    return std::ranges::find(kMath, name) != kMath.end();
}

} // namespace loxpp::tooling
