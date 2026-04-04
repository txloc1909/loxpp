#include "value.h"
#include "overload.h"

#include <cstdio>

bool operator!(Value value) {
    return std::visit(overloaded{
                          [](bool val) -> bool { return !val; },
                          [](Nil) -> bool { return true; },
                          [](ObjHandle) -> bool { return false; },
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
            // Interning guarantees: same content → same index → equal handles.
            [](ObjHandle a_h, ObjHandle b_h) -> bool {
                return a_h == b_h;
            },
            [](const auto&, const auto&) -> bool { return false; }},
        a, b);
}

void printValue(const Value& value, const Allocator& alloc) {
    std::printf("%s", stringify(value, alloc).c_str());
}

std::string stringify(const Value& value, const Allocator& alloc) {
    return std::visit(
        overloaded{
            [](bool val) -> std::string { return val ? "true" : "false"; },
            [](Number val) -> std::string {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g", val);
                return buf;
            },
            [](Nil) -> std::string { return "nil"; },
            [&alloc](ObjHandle h) -> std::string {
                return stringifyObj(alloc.deref(h));
            },
        },
        value);
}

void printValue(const Value& value) {
    std::printf("%s", stringify(value).c_str());
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
            [](ObjHandle h) -> std::string {
                return "<str#" + std::to_string(h.index) + ">";
            },
        },
        value);
}

uint8_t ValueArray::size() const { return m_count; }

void ValueArray::write(Value value) { at(m_count++) = value; }
