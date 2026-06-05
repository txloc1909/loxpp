#pragma once

#include "object.h"

#include <cstdint>
#include <variant>
#include <vector>

using Number = double;
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
