#pragma once

#include <array>
#include <cstdint>
#include <variant>

using Number = double;
using Nil = std::monostate;
using Value = std::variant<bool, Number, Nil>;

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

class ValueArray : std::array<Value, UINT8_MAX> {
  public:
    using std::array<Value, UINT8_MAX>::at;
    void write(Value value);
    uint8_t size() const;

  private:
    uint8_t m_count = 0;
};
