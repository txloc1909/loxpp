#include "integer_module.h"
#include "stdlib_context.h"
#include "../value.h"

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

// The `integer` module (spec/05-stdlib.md, integer). Every function takes Ints
// and every unsigned view reads the same 64 bits as a std::uint64_t, so the
// wrapping results are defined behaviour (docs/decisions/integer-kind.md,
// constraint 1).

namespace {

// Reads `count` arguments, each of which must be an Int. An integer-valued
// Number is not an Int, so it faults, as it does in the `math` module for the
// other direction.
bool readInts(Value* args, std::size_t count, Int* out) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!is<Int>(args[i])) {
            nativeRuntimeError("integer function argument must be an Int.");
            return false;
        }
        out[i] = as<Int>(args[i]);
    }
    return true;
}

Value absNative(int /*argCount*/, Value* args) {
    std::array<Int, 1> values{};
    if (!readInts(args, values.size(), values.data())) {
        return from<Nil>(Nil{});
    }
    // The negation of the smallest Int is itself, so abs() wraps with it.
    return makeIntValue(*getActiveMM(),
                        values[0] < 0 ? intWrapNegate(values[0]) : values[0]);
}

Value minNative(int /*argCount*/, Value* args) {
    std::array<Int, 2> values{};
    if (!readInts(args, values.size(), values.data())) {
        return from<Nil>(Nil{});
    }
    return makeIntValue(*getActiveMM(),
                        values[0] < values[1] ? values[0] : values[1]);
}

Value maxNative(int /*argCount*/, Value* args) {
    std::array<Int, 2> values{};
    if (!readInts(args, values.size(), values.data())) {
        return from<Nil>(Nil{});
    }
    return makeIntValue(*getActiveMM(),
                        values[0] > values[1] ? values[0] : values[1]);
}

Value udivNative(int /*argCount*/, Value* args) {
    std::array<Int, 2> values{};
    if (!readInts(args, values.size(), values.data())) {
        return from<Nil>(Nil{});
    }
    const auto divisor = static_cast<std::uint64_t>(values[1]);
    if (divisor == 0) {
        nativeRuntimeError("integer.udiv() divisor must not be zero.");
        return from<Nil>(Nil{});
    }
    const auto quotient = static_cast<std::uint64_t>(values[0]) / divisor;
    return makeIntValue(*getActiveMM(), std::bit_cast<Int>(quotient));
}

Value uremNative(int /*argCount*/, Value* args) {
    std::array<Int, 2> values{};
    if (!readInts(args, values.size(), values.data())) {
        return from<Nil>(Nil{});
    }
    const auto divisor = static_cast<std::uint64_t>(values[1]);
    if (divisor == 0) {
        nativeRuntimeError("integer.urem() divisor must not be zero.");
        return from<Nil>(Nil{});
    }
    const auto remainder = static_cast<std::uint64_t>(values[0]) % divisor;
    return makeIntValue(*getActiveMM(), std::bit_cast<Int>(remainder));
}

Value ultNative(int /*argCount*/, Value* args) {
    std::array<Int, 2> values{};
    if (!readInts(args, values.size(), values.data())) {
        return from<Nil>(Nil{});
    }
    return from<bool>(static_cast<std::uint64_t>(values[0]) <
                      static_cast<std::uint64_t>(values[1]));
}

Value ustrNative(int /*argCount*/, Value* args) {
    std::array<Int, 1> values{};
    if (!readInts(args, values.size(), values.data())) {
        return from<Nil>(Nil{});
    }
    const std::string digits =
        std::to_string(static_cast<std::uint64_t>(values[0]));
    return Value{static_cast<Obj*>(getActiveMM()->makeString(digits))};
}

// The String form of parse_unsigned(): one or more decimal digits, no sign and
// nothing else, with a value of at most 2^64 - 1 (spec/05-stdlib.md,
// integer).
Value parseUnsignedNative(int /*argCount*/, Value* args) {
    if (!isString(args[0])) {
        nativeRuntimeError(
            "integer.parse_unsigned() argument must be a String.");
        return from<Nil>(Nil{});
    }
    const LoxString& chars = asObjString(as<Obj*>(args[0]))->chars;
    std::uint64_t magnitude = 0;
    if (chars.empty()) {
        nativeRuntimeError(
            "integer.parse_unsigned() String must be decimal digits.");
        return from<Nil>(Nil{});
    }
    for (char c : chars) {
        if (c < '0' || c > '9') {
            nativeRuntimeError(
                "integer.parse_unsigned() String must be decimal digits.");
            return from<Nil>(Nil{});
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (magnitude >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            nativeRuntimeError(
                "integer.parse_unsigned() String is above 2^64 - 1.");
            return from<Nil>(Nil{});
        }
        magnitude = magnitude * 10 + digit;
    }
    return makeIntValue(*getActiveMM(), std::bit_cast<Int>(magnitude));
}

struct IntegerFnEntry {
    const char* name;
    NativeFn fn;
    int arity;
};

struct IntegerConstEntry {
    const char* name;
    Int value;
};

const IntegerFnEntry kIntegerFunctions[] = {
    {"abs", absNative, 1},   {"min", minNative, 2},
    {"max", maxNative, 2},   {"udiv", udivNative, 2},
    {"urem", uremNative, 2}, {"ult", ultNative, 2},
    {"ustr", ustrNative, 1}, {"parse_unsigned", parseUnsignedNative, 1},
};

const IntegerConstEntry kIntegerConstants[] = {
    {"min_value", std::numeric_limits<Int>::min()},
    {"max_value", std::numeric_limits<Int>::max()},
};

} // namespace

void registerInteger(StdlibRegistrar& reg) {
    ObjClass* klass = reg.makeClass("Integer");
    reg.mm().pushTempRoot(klass);
    ObjInstance* inst = reg.makeInstance(klass);
    reg.mm().pushTempRoot(inst);
    for (const IntegerFnEntry& fn : kIntegerFunctions) {
        reg.addNativeField(inst, fn.name, fn.fn, fn.arity);
    }
    for (const IntegerConstEntry& constant : kIntegerConstants) {
        reg.addField(inst, constant.name,
                     makeIntValue(reg.mm(), constant.value));
    }
    reg.defineGlobalValue("integer", Value{static_cast<Obj*>(inst)});
    reg.mm().popTempRoot(); // inst
    reg.mm().popTempRoot(); // klass
}
