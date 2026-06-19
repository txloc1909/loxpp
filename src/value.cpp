#include "value.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

bool operator!(Value value) {
    if (is<bool>(value)) {
        return !as<bool>(value);
    }
    return is<Nil>(value); // numbers and objects are truthy
}

bool operator==(const Value& a, const Value& b) {
    // Numbers must compare as IEEE 754 doubles (so +0.0 == -0.0 and NaN != NaN)
    // rather than by raw representation — handle them before anything else.
    if (is<Number>(a) && is<Number>(b)) {
        return as<Number>(a) == as<Number>(b);
    }
    if (is<Number>(a) || is<Number>(b)) {
        return false; // a number is never equal to a non-number
    }
    if (is<bool>(a) && is<bool>(b)) {
        return as<bool>(a) == as<bool>(b);
    }
    if (is<Nil>(a) && is<Nil>(b)) {
        return true;
    }
    // For strings, interning guarantees same content → same pointer. Other Obj
    // types use identity equality.
    if (is<Obj*>(a) && is<Obj*>(b)) {
        return as<Obj*>(a) == as<Obj*>(b);
    }
    return false;
}

std::string stringify(const Value& value) {
    if (is<bool>(value)) {
        return as<bool>(value) ? "true" : "false";
    }
    if (is<Nil>(value)) {
        return "nil";
    }
    if (is<Obj*>(value)) {
        return stringifyObj(as<Obj*>(value));
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", as<Number>(value));
    return buf;
}

void printValue(const Value& value) {
    std::printf("%s", stringify(value).c_str());
}

uint16_t ValueArray::size() const { return m_count; }

void ValueArray::write(Value value) {
    m_values.push_back(value);
    m_count++;
}

uint32_t hashValue(const Value& v) {
    if (is<bool>(v)) {
        return as<bool>(v) ? 1231U : 1237U;
    }
    if (is<Nil>(v)) {
        return 0U;
    }
    if (is<Obj*>(v)) {
        return static_cast<ObjString*>(as<Obj*>(v))->hash;
    }
    Number n = as<Number>(v);
    if (n == 0.0) {
        n = 0.0; // canonicalize -0.0 → +0.0
    }
    uint64_t bits;
    std::memcpy(&bits, &n, sizeof bits);
    return static_cast<uint32_t>(bits ^ (bits >> 32));
}

bool isValidMapKey(const Value& v) {
    if (is<Number>(v)) {
        return !std::isnan(as<Number>(v));
    }
    if (is<Obj*>(v)) {
        return as<Obj*>(v)->type == ObjType::STRING;
    }
    return true; // bool and nil are always valid
}

size_t ValueHash::operator()(const Value& v) const {
    if (is<bool>(v)) {
        return as<bool>(v) ? 1231U : 1237U;
    }
    if (is<Nil>(v)) {
        return 0U;
    }
    if (is<Obj*>(v)) {
        // Pointer identity — matches operator==. Do NOT use hashValue(), which
        // assumes Obj* is an interned ObjString and is UB for other objects.
        return std::hash<const void*>{}(as<Obj*>(v));
    }
    Number n = as<Number>(v);
    if (n == 0.0) {
        n = 0.0; // canonicalize -0.0 → +0.0 (matches -0.0 == 0.0)
    }
    uint64_t bits;
    std::memcpy(&bits, &n, sizeof bits);
    return std::hash<uint64_t>{}(bits);
}
