#include "tooling/stdlib_names.h"

#include <algorithm>
#include <array>

namespace loxpp::tooling {

namespace {

// From src/stdlib/globals.cpp (clock input str len), src/stdlib/file_api.cpp
// (open), src/stdlib/os_api.cpp (args env exit time sleep exists is_dir
// is_file stat), src/stdlib/reflect_api.cpp (type fields methods getField
// setField hasField callMethod), src/stdlib/math_module.cpp (math).
// `print` is a statement keyword, not a global, so it is not listed.
constexpr std::array<std::string_view, 22> kGlobals = {
    "clock",    "input",    "str",        "len",    "open",    "args",
    "env",      "exit",     "time",       "sleep",  "exists",  "is_dir",
    "is_file",  "stat",     "type",       "fields", "methods", "getField",
    "setField", "hasField", "callMethod", "math",
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
