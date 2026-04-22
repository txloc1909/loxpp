#ifdef LOXPP_NAN_TAGGING

#include "value.h"

#include <bit>
#include <cstdio>

bool operator!(Value v) {
    return v.bits == detail::VAL_FALSE || v.bits == detail::VAL_NIL;
}

bool operator==(const Value& a, const Value& b) {
    // Numbers must compare as IEEE 754 doubles so that:
    //   +0.0 == -0.0  (different bits, same double value)
    //   NaN  != NaN   (same bits, but IEEE 754 mandates NaN != NaN)
    if (is<Number>(a) && is<Number>(b))
        return std::bit_cast<double>(a.bits) == std::bit_cast<double>(b.bits);
    // For all other types the encoding is unique — same bits means equal.
    return a.bits == b.bits;
}

std::string stringify(const Value& value) {
    if (is<bool>(value))
        return as<bool>(value) ? "true" : "false";
    if (is<Nil>(value))
        return "nil";
    if (is<Obj*>(value))
        return stringifyObj(as<Obj*>(value));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", as<Number>(value));
    return buf;
}

void printValue(const Value& value) {
    std::printf("%s", stringify(value).c_str());
}

uint8_t ValueArray::size() const { return m_count; }

void ValueArray::write(Value value) { at(m_count++) = value; }

#endif // LOXPP_NAN_TAGGING
