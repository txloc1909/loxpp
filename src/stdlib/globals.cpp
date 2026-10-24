#include "globals.h"
#include "stdlib_context.h"
#include "../container_objects.h"
#include "../decimal_number.h"
#include "../exec_objects.h"
#include "../object.h"
#include "../value.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

static Value clockNative(int /*argCount*/, Value* /*args*/) {
    return from<Number>(static_cast<double>(std::clock()) / CLOCKS_PER_SEC);
}

// Byte value of a one-byte string. The argument is restricted to a single
// byte so ord() and chr() stay exact inverses over 0..255.
static Value ordNative(int /*argCount*/, Value* args) {
    Value v = args[0];
    if (!isString(v)) {
        nativeRuntimeError("ord() argument must be a string.");
        return from<Nil>(Nil{});
    }
    const LoxString& chars = asObjString(as<Obj*>(v))->chars;
    if (chars.size() != 1) {
        nativeRuntimeError("ord() argument must be a one-character string.");
        return from<Nil>(Nil{});
    }
    return from<Number>(
        static_cast<double>(static_cast<unsigned char>(chars[0])));
}

// One-byte string for a byte value. Requires an integral Number in 0..255 so
// it is the inverse of ord().
static Value chrNative(int /*argCount*/, Value* args) {
    Value v = args[0];
    if (!is<Number>(v)) {
        nativeRuntimeError("chr() argument must be a number.");
        return from<Nil>(Nil{});
    }
    Number n = as<Number>(v);
    if (std::floor(n) != n || n < 0 || n > 255) {
        nativeRuntimeError("chr() argument must be an integer from 0 to 255.");
        return from<Nil>(Nil{});
    }
    auto byte =
        static_cast<char>(static_cast<unsigned char>(static_cast<int>(n)));
    ObjString* s = getActiveMM()->makeString(std::string_view(&byte, 1));
    return Value{static_cast<Obj*>(s)};
}

// Read one line from stdin. Returns nil on EOF, otherwise an ObjString.
static Value inputNative(int /*argCount*/, Value* /*args*/) {
    std::string line;
    if (!std::getline(std::cin, line)) {
        return from<Nil>(Nil{});
    }
    ObjString* s = getActiveMM()->makeString(line);
    return Value{static_cast<Obj*>(s)};
}

// The two ends of the Int range as doubles. 2^63 is exact, so it is the first
// Number above the range; -2^63 is the smallest Int itself.
static constexpr double kTwoToThe63 = 9223372036854775808.0;

// The String form of int(): an optional '+' or '-' and one or more decimal
// digits, nothing else (spec/05-stdlib.md, int). The magnitude may reach 2^63
// only when the sign is negative, so the smallest Int parses and the same
// digits with a positive sign do not.
static bool parseIntString(std::string_view text, Int* out) {
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    const std::size_t firstDigit = i;
    const std::uint64_t limit =
        negative ? (std::uint64_t{1} << 63)
                 : static_cast<std::uint64_t>(std::numeric_limits<Int>::max());
    std::uint64_t magnitude = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(text[i] - '0');
        if (magnitude > (limit - digit) / 10) {
            return false;
        }
        magnitude = magnitude * 10 + digit;
    }
    if (i == firstDigit) {
        return false;
    }
    *out = negative ? std::bit_cast<Int>(std::uint64_t{0} - magnitude)
                    : static_cast<Int>(magnitude);
    return true;
}

// The String form of float(): an optional sign, one or more decimal digits,
// and optionally a '.' followed by one or more decimal digits, nothing else
// (spec/05-stdlib.md, float). The grammar is checked before the parse, so
// float("1e5") faults even though strtod() would accept it.
static bool parseFloatString(std::string_view text, Number* out) {
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        ++i;
    }
    const std::size_t firstDigit = i;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    }
    if (i == firstDigit) {
        return false;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        const std::size_t firstFractionDigit = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
        }
        if (i == firstFractionDigit) {
            return false;
        }
    }
    if (i != text.size()) {
        return false;
    }
    *out = nearestDoubleFromDecimal(text);
    return true;
}

// int(x): an Int gives itself, an integral Number in the Int range gives the
// equal Int, and a decimal String parses. It never rounds, so a fractional
// Number is a fault (spec/05-stdlib.md, int).
static Value intNative(int /*argCount*/, Value* args) {
    const Value v = args[0];
    if (is<Int>(v)) {
        return makeIntValue(*getActiveMM(), as<Int>(v));
    }
    if (is<Number>(v)) {
        const Number n = as<Number>(v);
        // Check the range before the cast: a cast of a NaN, an infinity, or a
        // Number outside the Int range is undefined behaviour. -0.0 passes
        // and becomes 0.
        if (!std::isfinite(n) || n != std::floor(n) || n < -kTwoToThe63 ||
            n >= kTwoToThe63) {
            nativeRuntimeError(
                "int() argument must be an integer-valued number in the range "
                "of an Int.");
            return from<Nil>(Nil{});
        }
        return makeIntValue(*getActiveMM(), static_cast<Int>(n));
    }
    if (isString(v)) {
        Int parsed = 0;
        const LoxString& chars = asObjString(as<Obj*>(v))->chars;
        if (!parseIntString(std::string_view{chars.data(), chars.size()},
                            &parsed)) {
            nativeRuntimeError(
                "int() String must be an optional sign and decimal digits in "
                "the range of an Int.");
            return from<Nil>(Nil{});
        }
        return makeIntValue(*getActiveMM(), parsed);
    }
    nativeRuntimeError("int() argument must be an Int, a Number, or a String.");
    return from<Nil>(Nil{});
}

// float(x): a Number gives itself, an Int gives the nearest double, and a
// decimal String parses. The Int conversion is static_cast, whose
// round-to-nearest-even matches spec/05-stdlib.md, float.
static Value floatNative(int /*argCount*/, Value* args) {
    const Value v = args[0];
    if (is<Number>(v)) {
        return from<Number>(as<Number>(v));
    }
    if (is<Int>(v)) {
        return from<Number>(static_cast<Number>(as<Int>(v)));
    }
    if (isString(v)) {
        Number parsed = 0.0;
        const LoxString& chars = asObjString(as<Obj*>(v))->chars;
        if (!parseFloatString(std::string_view{chars.data(), chars.size()},
                              &parsed)) {
            nativeRuntimeError(
                "float() String must be an optional sign, decimal digits, and "
                "an optional fractional part.");
            return from<Nil>(Nil{});
        }
        return from<Number>(parsed);
    }
    nativeRuntimeError(
        "float() argument must be an Int, a Number, or a String.");
    return from<Nil>(Nil{});
}

void registerGlobals(StdlibRegistrar& reg) {
    reg.defineGlobal("clock", clockNative, 0);
    reg.defineGlobal("input", inputNative, 0);
    reg.defineGlobal("ord", ordNative, 1);
    reg.defineGlobal("chr", chrNative, 1);
    reg.defineGlobal("int", intNative, 1);
    reg.defineGlobal("float", floatNative, 1);
}
