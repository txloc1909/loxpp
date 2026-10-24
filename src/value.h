#pragma once

#include "object.h"

#include <bit> // std::bit_cast, std::countr_zero
#include <cstdint>
#include <vector>

using Number = double;
using Int = int64_t;

// ---- NaN-boxing -----------------------------------------------------------
// All Value types are packed into a single 8-byte word using quiet-NaN bit
// patterns. A quiet NaN has the exponent all-ones (bits 62:52) and the quiet
// bit (bit 51) set; we additionally require bit 50, so QNAN = 0x7FFC...0000.
// This keeps a CPU-generated arithmetic NaN (0x7FF8...0000, bit 50 clear) out
// of our sentinel space, so it still classifies as a Number.
namespace detail {
inline constexpr uint64_t QNAN = 0x7FFC000000000000ULL;
inline constexpr uint64_t SIGN_BIT = 0x8000000000000000ULL;
inline constexpr uint64_t OBJ_TAG = QNAN | SIGN_BIT; // 0xFFFC000000000000
inline constexpr uint64_t VAL_NIL = QNAN | 0x01ULL;
inline constexpr uint64_t VAL_FALSE = QNAN | 0x02ULL;
inline constexpr uint64_t VAL_TRUE = QNAN | 0x03ULL;
// A global slot that has no binding yet (QBE backend, #592). It is not a Lox
// value and must never reach user code: the compiled read checks it and
// throws, and the declare/assign paths overwrite it. The payload 0x04 is
// unused by the tags above, and the QNAN bit 50 keeps a hardware NaN out.
inline constexpr uint64_t VAL_UNBOUND_GLOBAL = QNAN | 0x04ULL;

// An Int that fits inline (docs/decisions/integer-kind.md, Native layout):
// tag bit 49 of the positive quiet-NaN space and a signed 48-bit payload in
// bits 47..0. Bit 48 stays clear, reserved for a later UInt kind. The two tag
// bits are part of INT_TAG_MASK so a value with bit 48 set never reads as an
// Int, and a value with bit 49 clear is one of the tags above.
inline constexpr uint64_t INT_TAG = QNAN | 0x0002000000000000ULL;
inline constexpr uint64_t INT_TAG_MASK = 0xFFFF000000000000ULL;
inline constexpr uint64_t INT_PAYLOAD_MASK = 0x0000FFFFFFFFFFFFULL;
inline constexpr uint64_t INT_SIGN_BIT = 0x0000800000000000ULL;
inline constexpr int64_t INT_INLINE_MIN = -(int64_t{1} << 47);
inline constexpr int64_t INT_INLINE_MAX = (int64_t{1} << 47) - 1;

// The inline tag must not read as an object, a Number, or one of the tags
// above, or a value of one kind would classify as two.
static_assert((INT_TAG & INT_TAG_MASK) == INT_TAG, "INT_TAG bits stay clear");
static_assert((INT_TAG & QNAN) == QNAN, "an inline Int is never a Number");
static_assert((INT_TAG & OBJ_TAG) != OBJ_TAG,
              "an inline Int is never an object");
static_assert((VAL_NIL & INT_TAG_MASK) != INT_TAG &&
                  (VAL_FALSE & INT_TAG_MASK) != INT_TAG &&
                  (VAL_TRUE & INT_TAG_MASK) != INT_TAG &&
                  (VAL_UNBOUND_GLOBAL & INT_TAG_MASK) != INT_TAG,
              "no existing tag reads as an inline Int");
} // namespace detail

inline constexpr bool intFitsInline(Int v) {
    return v >= detail::INT_INLINE_MIN && v <= detail::INT_INLINE_MAX;
}

// The OBJ_TAG pattern leaves this many low bits for an object pointer. Every
// boxed Obj* must pass pointerFitsInValue() first; MemoryManager::create
// enforces that. Supported targets sit well inside the limit: x86-64 4-level
// paging and aarch64 48-bit VA keep user addresses below 2^48. Only opt-in
// wide-VA layouts (aarch64 52-bit LVA, x86-64 5-level paging) can exceed it,
// and no 8-byte box can then recover the pointer.
inline constexpr unsigned kPointerBits =
    static_cast<unsigned>(std::countr_zero(detail::OBJ_TAG));
static_assert(kPointerBits > 0 && kPointerBits < 64,
              "OBJ_TAG must leave 1..63 bits for an object pointer");
constexpr bool pointerFitsInValue(uintptr_t p) {
    return (p >> kPointerBits) == 0;
}
// Reports a pointer that does not fit and terminates. Defined in value.cpp.
[[noreturn]] void reportPointerOutOfRange(uintptr_t p);

struct Nil {};

// NaN-tagged Value: all types encoded in one 8-byte word.
//
// The double and bool constructors are intentionally non-explicit: the
// BINARY_OP macro in vm.cpp relies on `push(as<valueType>(a op b))`, where the
// raw bool/double result of `a op b` must implicitly convert to a Value to bind
// to as<T>(const Value&), and Value's own truthiness does the same. The Obj*
// constructor stays explicit; every object call site already uses
// Value{static_cast<Obj*>(p)}.
struct Value {
    uint64_t bits;

    Value() noexcept : bits(detail::VAL_NIL) {}
    Value(double d) noexcept : bits(std::bit_cast<uint64_t>(d)) {}
    Value(bool b) noexcept : bits(b ? detail::VAL_TRUE : detail::VAL_FALSE) {}
    Value(Nil) noexcept : bits(detail::VAL_NIL) {}
    explicit Value(Obj* p) noexcept
        : bits(detail::OBJ_TAG | reinterpret_cast<uint64_t>(p)) {}
};

static_assert(sizeof(double) == 8, "Platform must have 64-bit double");
static_assert(sizeof(uint64_t) == 8, "Platform must have 64-bit uint64_t");
static_assert(sizeof(Value) == 8, "NaN-boxed Value must be 8 bytes");

// The inline Int form. Precondition: intFitsInline(v). A value outside that
// range is an ObjInt in the heap; the compiler and the VM make that object
// (MemoryManager::create<ObjInt>), so an Int inside the range never allocates.
inline Value inlineIntValue(Int v) {
    Value value;
    value.bits =
        detail::INT_TAG | (static_cast<uint64_t>(v) & detail::INT_PAYLOAD_MASK);
    return value;
}

template <typename T>
bool is(const Value& v);
// True for the inline form only. The fast paths in the VM use it to tell an
// Int that needs no allocation from one that is a heap box (slow path).
inline bool isInlineInt(const Value& v) {
    return (v.bits & detail::INT_TAG_MASK) == detail::INT_TAG;
}
template <>
inline bool is<Number>(const Value& v) {
    return (v.bits & detail::QNAN) != detail::QNAN;
}
template <>
inline bool is<bool>(const Value& v) {
    return v.bits == detail::VAL_TRUE || v.bits == detail::VAL_FALSE;
}
template <>
inline bool is<Nil>(const Value& v) {
    return v.bits == detail::VAL_NIL;
}
template <>
inline bool is<Obj*>(const Value& v) {
    return (v.bits & detail::OBJ_TAG) == detail::OBJ_TAG;
}
template <>
inline bool is<Int>(const Value& v) {
    if ((v.bits & detail::INT_TAG_MASK) == detail::INT_TAG) {
        return true;
    }
    return (v.bits & detail::OBJ_TAG) == detail::OBJ_TAG &&
           reinterpret_cast<Obj*>(v.bits & ~detail::OBJ_TAG)->type ==
               ObjType::INT;
}

// ---- Int arithmetic -------------------------------------------------------
// Every wrapping operation runs in uint64_t and returns through bit_cast, so
// two's-complement wrap is defined behaviour and the debug preset's
// -fsanitize=undefined has nothing to report (docs/decisions/
// integer-kind.md, constraint 1).
//
// Division and modulo take the floor rule of spec/04-semantics.md,
// Arithmetic. Their preconditions are b != 0 and, for the two defined edge
// rows, a == INT64_MIN with b == -1: the callers check both before calling,
// because only they know how to raise the fault or return the edge result.
inline Int intWrapAdd(Int a, Int b) {
    return std::bit_cast<Int>(static_cast<uint64_t>(a) +
                              static_cast<uint64_t>(b));
}
inline Int intWrapSubtract(Int a, Int b) {
    return std::bit_cast<Int>(static_cast<uint64_t>(a) -
                              static_cast<uint64_t>(b));
}
inline Int intWrapMultiply(Int a, Int b) {
    return std::bit_cast<Int>(static_cast<uint64_t>(a) *
                              static_cast<uint64_t>(b));
}
inline Int intWrapNegate(Int a) {
    return std::bit_cast<Int>(uint64_t{0} - static_cast<uint64_t>(a));
}
inline Int intFloorDivide(Int a, Int b) {
    Int quotient = a / b;
    Int remainder = a % b;
    if (remainder != 0 && ((remainder < 0) != (b < 0))) {
        --quotient;
    }
    return quotient;
}
inline Int intFloorModulo(Int a, Int b) {
    Int remainder = a % b;
    if (remainder != 0 && ((remainder < 0) != (b < 0))) {
        remainder += b;
    }
    return remainder;
}
inline Int intShiftLeft(Int a, Int count) {
    return std::bit_cast<Int>(static_cast<uint64_t>(a)
                              << static_cast<unsigned>(count));
}
inline Int intShiftRight(Int a, Int count) {
    return a >> static_cast<unsigned>(count);
}
inline Int intShiftRightUnsigned(Int a, Int count) {
    return std::bit_cast<Int>(static_cast<uint64_t>(a) >>
                              static_cast<unsigned>(count));
}
inline Int intBitAnd(Int a, Int b) {
    return std::bit_cast<Int>(static_cast<uint64_t>(a) &
                              static_cast<uint64_t>(b));
}
inline Int intBitOr(Int a, Int b) {
    return std::bit_cast<Int>(static_cast<uint64_t>(a) |
                              static_cast<uint64_t>(b));
}
inline Int intBitXor(Int a, Int b) {
    return std::bit_cast<Int>(static_cast<uint64_t>(a) ^
                              static_cast<uint64_t>(b));
}
inline Int intBitNot(Int a) {
    return std::bit_cast<Int>(~static_cast<uint64_t>(a));
}

// A global slot's empty state (src/runtime.h's m_globalSlots). Not one of the
// Lox value types, so `is<T>` above all return false for it.
inline bool isUnboundGlobal(const Value& v) {
    return v.bits == detail::VAL_UNBOUND_GLOBAL;
}
inline Value unboundGlobal() {
    Value v;
    v.bits = detail::VAL_UNBOUND_GLOBAL;
    return v;
}

// as<T> performs no type check; every call site guards with the matching
// is<T> first.
template <typename T>
T as(const Value& v);
template <>
inline Number as<Number>(const Value& v) {
    return std::bit_cast<double>(v.bits);
}
template <>
inline bool as<bool>(const Value& v) {
    return v.bits == detail::VAL_TRUE;
}
template <>
inline Nil as<Nil>(const Value&) {
    return {};
}
template <>
inline Obj* as<Obj*>(const Value& v) {
    return reinterpret_cast<Obj*>(v.bits & ~detail::OBJ_TAG);
}
// No type check: every call site guards with is<Int>. A boxed Int is read
// through its own field, so equality, hashing and printing see the value and
// never the address.
template <>
inline Int as<Int>(const Value& v) {
    if ((v.bits & detail::INT_TAG_MASK) == detail::INT_TAG) {
        int64_t raw = static_cast<int64_t>(v.bits & detail::INT_PAYLOAD_MASK);
        if ((raw & static_cast<int64_t>(detail::INT_SIGN_BIT)) != 0) {
            raw |= ~static_cast<int64_t>(detail::INT_PAYLOAD_MASK);
        }
        return raw;
    }
    return static_cast<ObjInt*>(as<Obj*>(v))->value;
}

template <typename T>
Value from(const T& val);
template <>
inline Value from<Number>(const Number& val) {
    return Value{val};
}
template <>
inline Value from<bool>(const bool& val) {
    return Value{val};
}
template <>
inline Value from<Nil>(const Nil&) {
    return Value{Nil{}};
}
template <>
inline Value from<Obj*>(Obj* const& val) {
    return Value{val};
}

template <ObjType T>
inline bool isValueOfType(const Value& v) {
    return is<Obj*>(v) && as<Obj*>(v)->type == T;
}

inline bool isObj(const Value& v) { return is<Obj*>(v); }
inline bool isString(const Value& v) {
    return isValueOfType<ObjType::STRING>(v);
}
inline bool isFunction(const Value& v) {
    return isValueOfType<ObjType::FUNCTION>(v);
}
inline bool isNative(const Value& v) {
    return isValueOfType<ObjType::NATIVE>(v);
}
inline bool isBoundNative(const Value& v) {
    return isValueOfType<ObjType::BOUND_NATIVE>(v);
}
// A callable value: a user function (closure), a native function, a bound
// user method, or a bound native method. This is the set type() reports as
// "Function" or "BoundMethod" (spec/05-stdlib.md, type): the NATIVE/CLOSURE/
// BOUND_NATIVE split is a C++ representation detail, not part of the
// language's type vocabulary (spec/03-types.md has one Function heading and
// one BoundMethod heading).
inline bool isCallableValue(const Value& v) {
    return isValueOfType<ObjType::CLOSURE>(v) || isNative(v) ||
           isValueOfType<ObjType::BOUND_METHOD>(v) || isBoundNative(v);
}
inline Obj* asObj(const Value& v) { return as<Obj*>(v); }
inline ObjString* asObjString(const Value& v) {
    return static_cast<ObjString*>(as<Obj*>(v));
}

bool operator!(Value value);
bool operator==(const Value& a, const Value& b);
std::string stringify(const Value& value);
void printValue(const Value& value);

// Hash a map-valid Value. Undefined behaviour if called with an invalid key.
uint32_t hashValue(const Value& v);
// Returns false for NaN numbers and non-string objects.
bool isValidMapKey(const Value& v);

// Hash for any Value of one heap, consistent with operator== on such values.
// Unlike hashValue() (the table's uint32 string-key hash), this hashes Obj*
// by pointer identity, so it is safe for any value, including non-string
// objects. Two Strings of two heaps can be == and still hash apart, so the
// values must all be of one heap, as in the compiler's constant pool, the
// one user.
// Implemented on is<T>/as<T>, not on the underlying representation.
struct ValueHash {
    size_t operator()(const Value& v) const;
};

inline bool isFalsy(const Value& v) { return !v; }

class ValueArray {
  public:
    void write(Value value);
    [[nodiscard]] uint16_t size() const;
    [[nodiscard]] bool isFull() const { return m_count >= UINT16_MAX; }
    [[nodiscard]] Value at(uint16_t idx) const { return m_values.at(idx); }

  private:
    std::vector<Value> m_values;
    uint16_t m_count = 0;
};
