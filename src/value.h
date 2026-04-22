#pragma once

#include "object.h"

#include <array>
#include <cstdint>

using Number = double;

#ifdef LOXPP_NAN_TAGGING

#include <bit>
#include <variant> // for std::monostate (Nil)

// ---- NaN-boxing constants ------------------------------------------------
// A quiet NaN has exponent all-ones (bits 62:52) and quiet bit (bit 51) set.
// We additionally require bit 50, giving QNAN = 0x7FFC000000000000. This
// ensures that CPU-generated arithmetic NaN (0x7FF8000000000000) is NOT
// caught by our sentinel test and still passes as a Number value.
namespace detail {
static constexpr uint64_t QNAN = 0x7FFC000000000000ULL;
static constexpr uint64_t SIGN_BIT = 0x8000000000000000ULL;
static constexpr uint64_t OBJ_TAG = QNAN | SIGN_BIT; // 0xFFFC000000000000

static constexpr uint64_t VAL_NIL = QNAN | 0x01ULL;
static constexpr uint64_t VAL_FALSE = QNAN | 0x02ULL;
static constexpr uint64_t VAL_TRUE = QNAN | 0x03ULL;
} // namespace detail

using Nil = std::monostate;

// NaN-tagged Value: all types encoded in a single 8-byte word.
//
// Constructors for double and bool are intentionally non-explicit so that
// the existing BINARY_OP macro in vm.cpp (`push(as<valueType>(a op b))`)
// compiles without change — `bool` produced by a comparison implicitly
// converts to Value via the bool constructor, exactly as it did with the
// old std::variant (which was also implicitly constructible from bool).
struct Value {
    uint64_t bits;

    Value() noexcept : bits(detail::VAL_NIL) {}
    Value(double d) noexcept : bits(std::bit_cast<uint64_t>(d)) {}
    Value(bool b) noexcept : bits(b ? detail::VAL_TRUE : detail::VAL_FALSE) {}
    Value(std::monostate) noexcept : bits(detail::VAL_NIL) {}
    explicit Value(Obj* p) noexcept
        : bits(detail::OBJ_TAG | reinterpret_cast<uint64_t>(p)) {}

    static_assert(sizeof(double) == 8 && sizeof(uint64_t) == 8,
                  "Platform must have 64-bit doubles and 64-bit pointers");
};

static_assert(sizeof(Value) == 8, "NaN-boxed Value must be 8 bytes");

// ---- is<T> ---------------------------------------------------------------
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

// ---- as<T> ---------------------------------------------------------------
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

// ---- from<T> -------------------------------------------------------------
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

#else // !LOXPP_NAN_TAGGING — original std::variant implementation

#include <variant>

using Nil = std::monostate;
using Value = std::variant<bool, Number, Nil, Obj*>;

template <typename T>
bool is(const Value& value) {
    return std::holds_alternative<T>(value);
}

template <typename T>
T as(const Value& value) {
    if (!is<T>(value))
        throw std::bad_variant_access();
    return std::get<T>(value);
}

template <typename T>
Value from(const T& val) {
    return Value(val);
}

#endif // LOXPP_NAN_TAGGING

// ---- inline helpers (same in both paths, built on is<>/as<>) -------------
inline bool isObj(const Value& v) { return is<Obj*>(v); }
inline bool isString(const Value& v) {
    return is<Obj*>(v) && as<Obj*>(v)->type == ObjType::STRING;
}
inline bool isFunction(const Value& v) {
    return is<Obj*>(v) && as<Obj*>(v)->type == ObjType::FUNCTION;
}
inline bool isNative(const Value& v) {
    return is<Obj*>(v) && as<Obj*>(v)->type == ObjType::NATIVE;
}
inline Obj* asObj(const Value& v) { return as<Obj*>(v); }
inline ObjString* asObjString(const Value& v) {
    return static_cast<ObjString*>(as<Obj*>(v));
}

bool operator!(Value value);
bool operator==(const Value& a, const Value& b);
std::string stringify(const Value& value);
void printValue(const Value& value);

inline bool isFalsy(const Value& v) { return !v; }

class ValueArray : std::array<Value, UINT8_MAX> {
  public:
    using std::array<Value, UINT8_MAX>::at;
    void write(Value value);
    uint8_t size() const;
    bool isFull() const { return m_count >= UINT8_MAX; }

  private:
    uint8_t m_count = 0;
};
