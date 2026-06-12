#pragma once

#include "object.h"

#include <cstdint>
#include <vector>

using Number = double;

#ifdef LOXPP_NAN_TAGGING

#include <bit>     // std::bit_cast
#include <variant> // std::monostate (Nil)

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
} // namespace detail

using Nil = std::monostate;

// NaN-tagged Value: all types encoded in one 8-byte word.
//
// The double and bool constructors are intentionally non-explicit: the
// BINARY_OP macro in vm.cpp relies on `push(as<valueType>(a op b))`, where the
// raw bool/double result of `a op b` must implicitly convert to a Value to bind
// to as<T>(const Value&). The std::variant representation was likewise
// implicitly constructible from bool/double. The Obj* constructor stays
// explicit; every object call site already uses Value{static_cast<Obj*>(p)}.
struct Value {
    uint64_t bits;

    Value() noexcept : bits(detail::VAL_NIL) {}
    Value(double d) noexcept : bits(std::bit_cast<uint64_t>(d)) {}
    Value(bool b) noexcept : bits(b ? detail::VAL_TRUE : detail::VAL_FALSE) {}
    Value(std::monostate) noexcept : bits(detail::VAL_NIL) {}
    explicit Value(Obj* p) noexcept
        : bits(detail::OBJ_TAG | reinterpret_cast<uint64_t>(p)) {}
};

static_assert(sizeof(double) == 8, "Platform must have 64-bit double");
static_assert(sizeof(uint64_t) == 8, "Platform must have 64-bit uint64_t");
static_assert(sizeof(Value) == 8, "NaN-boxed Value must be 8 bytes");

template <typename T>
bool is(const Value& v);
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

// as<T> performs no type check (unlike the throwing std::variant version);
// every call site guards with the matching is<T> first.
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
    return Value{std::monostate{}};
}
template <>
inline Value from<Obj*>(Obj* const& val) {
    return Value{val};
}

#else // !LOXPP_NAN_TAGGING — portable std::variant representation

#include <variant>

using Nil = std::monostate;
using Value = std::variant<bool, Number, Nil, Obj*>;

template <typename T>
bool is(const Value& value) {
    return std::holds_alternative<T>(value);
}

template <typename T>
T as(const Value& value) {
    if (!is<T>(value)) {
        throw std::bad_variant_access();
    }
    return std::get<T>(value);
}

template <typename T>
Value from(const T& val) {
    return Value(val);
}

#endif // LOXPP_NAN_TAGGING

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
