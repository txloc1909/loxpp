#include "value.h"
#include "overload.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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
            // For strings, interning guarantees same content → same pointer.
            // Other Obj types use identity equality.
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

uint32_t hashValue(const Value& v) {
    return std::visit(
        overloaded{
            [](bool b) -> uint32_t { return b ? 1231U : 1237U; },
            [](Number n) -> uint32_t {
                if (n == 0.0)
                    n = 0.0; // canonicalize -0.0 → +0.0
                uint64_t bits;
                std::memcpy(&bits, &n, sizeof bits);
                return static_cast<uint32_t>(bits ^ (bits >> 32));
            },
            [](Nil) -> uint32_t { return 0U; },
            [](Obj* p) -> uint32_t { return static_cast<ObjString*>(p)->hash; },
        },
        v);
}

bool isValidMapKey(const Value& v) {
    if (is<Number>(v))
        return !std::isnan(as<Number>(v));
    if (is<Obj*>(v))
        return as<Obj*>(v)->type == ObjType::STRING;
    return true; // bool and nil are always valid
}
