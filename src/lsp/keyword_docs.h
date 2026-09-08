#pragma once

// One-line documentation for every Lox++ keyword. The keyword set is the one
// in src/scanner.cpp lox_keywords() and spec/01-lexical.md; `case` and
// `default` are keyword tokens too and are listed here.

#include <array>
#include <string_view>
#include <utility>

namespace loxpp::lsp {

inline constexpr std::array<std::pair<std::string_view, std::string_view>, 24>
    kKeywordDocs = {{
        {"and", "Logical conjunction. Evaluates the right side only when the "
                "left side is truthy."},
        {"break", "Exits the innermost loop, or the innermost `match` arm "
                  "body."},
        {"case", "Starts a `match` arm: `case <pattern> => <body>`."},
        {"class", "Declares a class. A class body holds method declarations."},
        {"continue", "Skips to the next iteration of the innermost loop."},
        {"default", "Reserved keyword. No grammar rule uses it; a `match` "
                    "fallback arm is `case _`."},
        {"else", "The alternate branch of an `if` statement."},
        {"enum", "Declares an enum with named constructors, used in `match` "
                 "patterns. Must be at global scope."},
        {"false", "The boolean literal false."},
        {"for", "A C-style `for` loop, or `for (var x in iterable)` over a "
                "list, string, or map."},
        {"fun", "Declares a function."},
        {"if", "Runs its body when the condition is truthy; an optional "
               "`else` branch runs otherwise."},
        {"in", "The `for-in` loop variable binding, and the sequence "
               "membership test operator."},
        {"match", "A match expression: tests a subject against patterns and "
                  "evaluates the first matching arm body."},
        {"nil", "The nil literal: the absence of a value."},
        {"or", "Logical disjunction. Evaluates the right side only when the "
               "left side is falsy."},
        {"print", "Writes a value and a newline to standard output."},
        {"return", "Returns from the enclosing function, with an optional "
                   "value."},
        {"super", "Inside a method, accesses a method of the superclass."},
        {"this", "Inside a method, the instance the method was called on."},
        {"true", "The boolean literal true."},
        {"var", "Declares a variable. Also `var [a, b] = ...` and "
                "`var {a, b} = ...` destructuring."},
        {"while", "Repeats its body while the condition is truthy."},
        {"_", "The `match` wildcard pattern: matches any value and binds "
              "nothing. An ordinary identifier elsewhere."},
    }};

// Returns the doc line for `word`, or an empty view when `word` is not a
// keyword.
[[nodiscard]] inline std::string_view keywordDoc(std::string_view word) {
    for (const auto& [name, doc] : kKeywordDocs) {
        if (name == word) {
            return doc;
        }
    }
    return {};
}

} // namespace loxpp::lsp
