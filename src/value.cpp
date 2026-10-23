#include "value.h"

#include <charconv>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>

void reportPointerOutOfRange(uintptr_t p) {
    std::fprintf(stderr,
                 "loxpp: object pointer 0x%" PRIxPTR
                 " needs more than the %u bits NaN-boxing reserves for a heap "
                 "pointer; this platform's user address space is wider than "
                 "the Value representation supports\n",
                 p, kPointerBits);
    std::abort();
}

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
    // An Int and a Number are never equal, so the two checks above already
    // answer every mixed pair. Two Ints compare by value, whether one of them
    // is heap-held or both are inline.
    if (is<Int>(a) && is<Int>(b)) {
        return as<Int>(a) == as<Int>(b);
    }
    if (is<Int>(a) || is<Int>(b)) {
        return false;
    }
    if (is<bool>(a) && is<bool>(b)) {
        return as<bool>(a) == as<bool>(b);
    }
    if (is<Nil>(a) && is<Nil>(b)) {
        return true;
    }
    // For strings, interning guarantees same content → same pointer inside
    // one heap. A string of another heap (a field of a message from another
    // worker) can have the characters of a local one, so two strings of two
    // heaps compare their characters. Other Obj types use identity equality.
    if (is<Obj*>(a) && is<Obj*>(b)) {
        Obj* x = as<Obj*>(a);
        Obj* y = as<Obj*>(b);
        if (x == y) {
            return true;
        }
        if (x->heapId == y->heapId || x->type != ObjType::STRING ||
            y->type != ObjType::STRING) {
            return false;
        }
        const auto* s = static_cast<const ObjString*>(x);
        const auto* t = static_cast<const ObjString*>(y);
        return s->hash == t->hash && s->chars.size() == t->chars.size() &&
               std::memcmp(s->chars.data(), t->chars.data(), s->chars.size()) ==
                   0;
    }
    return false;
}

// The canonical string form of a Number: the shortest decimal that round-trips
// to the same IEEE 754 double, presented as ECMAScript Number::toString
// (radix 10) with Lox++'s lower-case special-value names. See
// docs/decisions/number-print-format.md.
static std::string stringifyNumber(Number value) {
    if (std::isnan(value)) {
        return std::signbit(value) ? "-nan" : "nan";
    }
    if (std::isinf(value)) {
        return value > 0 ? "inf" : "-inf";
    }
    if (value == 0.0) {
        return "0"; // covers -0.0
    }

    const bool negative = value < 0;
    const double mag = negative ? -value : value;

    // The shortest digits in scientific form d[.ddd]e±NN. A 64-byte buffer is
    // ample for any double, so to_chars cannot fail.
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), mag,
                             std::chars_format::scientific);
    const std::string_view sci(buf, static_cast<size_t>(res.ptr - buf));
    const size_t epos = sci.find('e');

    std::string digits;
    digits.reserve(sci.size());
    for (char c : sci.substr(0, epos)) {
        if (c != '.') {
            digits.push_back(c);
        }
    }

    const char* exp = sci.data() + epos + 1;
    if (*exp == '+') {
        ++exp; // from_chars accepts a '-' but not a '+'
    }
    int exponent = 0;
    std::from_chars(exp, sci.data() + sci.size(), exponent);

    const int k = static_cast<int>(digits.size());
    const int n = exponent + 1; // the decimal point position, as in ECMAScript

    std::string result;
    result.reserve(digits.size() + 8);
    if (negative) {
        result.push_back('-');
    }
    if (k <= n && n <= 21) {
        result += digits;
        result.append(static_cast<size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        result.append(digits, 0, static_cast<size_t>(n));
        result.push_back('.');
        result.append(digits, static_cast<size_t>(n), std::string::npos);
    } else if (-6 < n && n <= 0) {
        result += "0.";
        result.append(static_cast<size_t>(-n), '0');
        result += digits;
    } else {
        result.push_back(digits[0]);
        if (k > 1) {
            result.push_back('.');
            result.append(digits, 1, std::string::npos);
        }
        result.push_back('e');
        const int sciExp = n - 1;
        if (sciExp >= 0) {
            result.push_back('+');
        }
        result += std::to_string(sciExp);
    }
    return result;
}

std::string stringify(const Value& value) {
    if (is<bool>(value)) {
        return as<bool>(value) ? "true" : "false";
    }
    if (is<Nil>(value)) {
        return "nil";
    }
    // Before the Obj* branch: a heap-held Int is an object, and it prints by
    // value, never as the address behind the tag.
    if (is<Int>(value)) {
        return std::to_string(as<Int>(value));
    }
    if (is<Obj*>(value)) {
        return stringifyObj(as<Obj*>(value));
    }
    return stringifyNumber(as<Number>(value));
}

void printValue(const Value& value) {
    std::string s = stringify(value);
    std::fwrite(s.data(), 1, s.size(), stdout);
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
    // An Int hashes by value and never as an ObjString pointer. It needs no
    // shared hash with the equal Number, because the two kinds are never equal
    // (docs/decisions/integer-kind.md, Equality, ordering and hashing).
    if (is<Int>(v)) {
        const uint64_t bits = static_cast<uint64_t>(as<Int>(v));
        return static_cast<uint32_t>(bits ^ (bits >> 32));
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
    // No Int is NaN, so every Int is a valid key (docs/decisions/
    // integer-kind.md, Equality, ordering and hashing).
    if (is<Int>(v)) {
        return true;
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
    // By value, never by address: it agrees with operator== for an inline and
    // a heap-held Int alike, including in the compiler's constant pool.
    if (is<Int>(v)) {
        return std::hash<uint64_t>{}(static_cast<uint64_t>(as<Int>(v)));
    }
    if (is<Obj*>(v)) {
        // Pointer identity. It agrees with operator== only for objects of one
        // heap: two Strings of two heaps can be == and hash apart. Do NOT use
        // hashValue(), which assumes Obj* is an ObjString and is UB for other
        // objects.
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
