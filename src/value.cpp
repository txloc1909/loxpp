#include "value.h"
#include "overload.h"

#include <cstdio>

bool operator!(Value value) {
    return std::visit(overloaded{
                          [](bool val) -> bool { return !val; },
                          [](Nil) -> bool { return true; },
                          [](Obj*) -> bool { return false; },
                          [](const auto&) -> bool { return false; },
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
            [](Obj* a_obj, Obj* b_obj) -> bool { return a_obj == b_obj; },
            [](const auto&, const auto&) -> bool { return false; }},
        a, b);
}

void printValue(const Value& value) {
    std::printf("%s", stringify(value).c_str());
}

std::string stringify(const Value& value) {
    return std::visit(
        overloaded{
            [](bool val) -> std::string { return val ? "true" : "false"; },
            [](Number val) -> std::string {
                // Match Lox's %g formatting: no trailing zeros, compact
                // exponent.
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g", val);
                return buf;
            },
            [](Nil) -> std::string { return "nil"; },
            [](Obj* obj) -> std::string { return stringifyObj(obj); },
        },
        value);
}

uint8_t ValueArray::size() const { return m_count; }

void ValueArray::write(Value value) { at(m_count++) = value; }
