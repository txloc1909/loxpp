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
            // Interning guarantees: same content → same pointer → equal.
            [](Obj* a_p, Obj* b_p) -> bool { return a_p == b_p; },
            [](const auto&, const auto&) -> bool { return false; }},
        a, b);
}

std::string stringify(const Value& value) {
    return std::visit(
        overloaded{
            [](bool val) -> std::string { return val ? "true" : "false"; },
            [](Number val) -> std::string {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g", val);
                return buf;
            },
            [](Nil) -> std::string { return "nil"; },
            [](Obj* p) -> std::string { return stringifyObj(p); },
        },
        value);
}

void printValue(const Value& value) {
    std::printf("%s", stringify(value).c_str());
}

uint8_t ValueArray::size() const { return m_count; }

void ValueArray::write(Value value) { at(m_count++) = value; }
