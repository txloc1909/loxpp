#pragma once

#include "object.h"

#include <array>
#include <cstdint>
#include <variant>

using Number = double;
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

// Hash a map-valid Value. Undefined behaviour if called with an invalid key.
uint32_t hashValue(const Value& v);
// Returns false for NaN numbers and non-string objects.
bool isValidMapKey(const Value& v);

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
