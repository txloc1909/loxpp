#pragma once

#include "value.h"

#include <charconv>
#include <limits>
#include <string_view>
#include <system_error>

// The nearest IEEE 754 double to a decimal text of one or more digits with an
// optional '.' and more digits, and an optional leading sign
// (spec/01-lexical.md, the range of a Number literal; spec/05-stdlib.md,
// float). A decimal above the largest finite double gives an infinity and one
// below the smallest subnormal gives zero. The caller checks the grammar: this
// function reads the text as far as its form allows and never faults.
//
// std::from_chars reads the digits itself, so the process locale's decimal
// point never decides where the fraction starts, and a long literal costs one
// pass.
inline Number nearestDoubleFromDecimal(std::string_view text) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1); // std::from_chars takes '-' but not '+'
    }
    const bool negative = !text.empty() && text.front() == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    Number parsed = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        parsed, std::chars_format::fixed);
    if (result.ec != std::errc::result_out_of_range) {
        return negative ? -parsed : parsed;
    }
    // A non-zero digit before the point means a value above the largest finite
    // double, because the parse would otherwise have succeeded. Digits only
    // after the point mean a value below the smallest subnormal.
    for (const char c : text) {
        if (c == '.') {
            break;
        }
        if (c != '0') {
            const Number large = std::numeric_limits<Number>::infinity();
            return negative ? -large : large;
        }
    }
    return negative ? -0.0 : 0.0;
}
