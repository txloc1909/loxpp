#include "value.h"
#include "overload.h"

#include <cstdio>

bool operator!(Value value) {
    return std::visit(overloaded{
                          [](bool val) -> bool { return !val; },
                          [](Nil) -> bool { return !false; },
                          [](const auto& val) -> bool { return !true; },
                      },
                      value);
}

bool operator==(const Value& a, const Value& b) {
    if (a.index() != b.index())
        return false;

    return std::visit(
        overloaded{
            [](bool a_val, bool b_val) -> bool { return a_val == b_val; },
            [](Number a_val, Number b_val) -> bool { return a_val == b_val; },
            [](Nil, Nil) -> bool { return true; },
            [](const auto&, const auto&) -> bool {
                // unreachable
                return false;
            }},
        a, b);
}

void printValue(const Value& value) {
    std::visit(
        overloaded{
            [](bool val) { std::printf("%s", (val ? "true" : "false")); },
            [](Number val) { std::printf("%g", val); },
            [](Nil) { std::printf("nil"); },
        },
        value);
}

uint8_t ValueArray::size() const { return m_count; }

void ValueArray::write(Value value) { at(m_count++) = value; }
