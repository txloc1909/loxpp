#pragma once

#include "allocator.h"
#include "object.h"

#include <array>
#include <cstdint>
#include <variant>

using Number = double;
using Nil = std::monostate;
// ObjHandle is a lightweight, stable index into the Allocator's object store.
// It does not dangle when the allocator moves/compacts objects.
using Value = std::variant<bool, Number, Nil, ObjHandle>;

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

inline bool isObj(const Value& v) { return is<ObjHandle>(v); }
inline bool isString(const Value& v) {
    return is<ObjHandle>(v) && as<ObjHandle>(v).type == ObjType::STRING;
}
inline Obj* asObj(const Value& v, const Allocator& alloc) {
    return alloc.deref(as<ObjHandle>(v));
}
inline ObjString* asObjString(const Value& v, const Allocator& alloc) {
    return static_cast<ObjString*>(alloc.deref(as<ObjHandle>(v)));
}

bool operator!(Value value);
bool operator==(const Value& a, const Value& b);
// Primary: dereferences ObjHandle via allocator for correct string output.
std::string stringify(const Value& value, const Allocator& alloc);
void printValue(const Value& value, const Allocator& alloc);
// Debug/trace: ObjHandle prints as '<str#N>' without dereferencing.
std::string stringify(const Value& value);
void printValue(const Value& value);

class ValueArray : std::array<Value, UINT8_MAX> {
  public:
    using std::array<Value, UINT8_MAX>::at;
    void write(Value value);
    uint8_t size() const;

  private:
    uint8_t m_count = 0;
};
