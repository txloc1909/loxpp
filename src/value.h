#pragma once

#include "object.h"

#include <array>
#include <cstdint>
#include <variant>

using Number = double;
using Nil = std::monostate;
// Obj* is non-owning. Lifetime guaranteed by VM's std::vector<std::unique_ptr<Obj>>.
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

inline bool isObj(const Value& v)    { return is<Obj*>(v); }
inline bool isString(const Value& v) { return isObj(v) && isObjType(as<Obj*>(v), ObjType::STRING); }
inline Obj* asObj(const Value& v)    { return as<Obj*>(v); }
inline ObjString* asObjString(const Value& v) { return static_cast<ObjString*>(as<Obj*>(v)); }
inline const char* asCString(const Value& v)  { return asObjString(v)->chars.c_str(); }

bool operator!(Value value);
bool operator==(const Value& a, const Value& b);
void printValue(const Value& value);

class ValueArray : std::array<Value, UINT8_MAX> {
  public:
    using std::array<Value, UINT8_MAX>::at;
    void write(Value value);
    uint8_t size() const;

  private:
    uint8_t m_count = 0;
};
