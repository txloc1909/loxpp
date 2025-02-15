#pragma once

#include <array>
#include <cstdint>

using Value = double;

class ValueArray : std::array<Value, UINT8_MAX> {
  public:
    using std::array<Value, UINT8_MAX>::at;
    void write(Value value);
    uint8_t size() const;

  private:
    uint8_t m_count = 0;
};
