#pragma once

#include "value.h"

#include <cstdint>
#include <optional>
#include <vector>
#include <utility>

using Byte = uint8_t;

enum class Op : Byte {
    CONSTANT,
    NIL,
    TRUE,
    FALSE,
    EQUAL,
    GREATER,
    LESS,
    NEGATE,
    ADD,
    SUBTRACT,
    MULTIPLY,
    DIVIDE,
    NOT,
    PRINT,
    POP,
    DISCARD, // like POP but for scope cleanup; does not update lastResult
    GET_LOCAL,
    SET_LOCAL,
    DEFINE_GLOBAL,
    GET_GLOBAL,
    SET_GLOBAL,
    RETURN,
};

inline Op toOpcode(Byte byte) { return static_cast<Op>(byte); }

class Chunk : std::vector<Byte> {
  public:
    using std::vector<Byte>::at;
    using std::vector<Byte>::data;
    using std::vector<Byte>::const_iterator;
    using std::vector<Byte>::cbegin;
    using std::vector<Byte>::cend;
    using std::vector<Byte>::size;

    void write(Byte byte, int line);
    void write(Op op, int line);
    std::optional<uint8_t> addConstant(Value value);
    Value getConstant(int idx) const;
    int getLine(int offset) const;

  private:
    ValueArray m_constants;
    std::vector<std::pair<int, int>> m_lines;
};
