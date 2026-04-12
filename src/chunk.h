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
    GET_LOCAL,
    SET_LOCAL,
    DEFINE_GLOBAL,
    GET_GLOBAL,
    SET_GLOBAL,
    JUMP,
    JUMP_IF_FALSE,
    LOOP,
    CALL,
    RETURN,
    CLOSURE,
    GET_UPVALUE,
    SET_UPVALUE,
    CLOSE_UPVALUE,
    CLASS,
    GET_PROPERTY,
    SET_PROPERTY,
    DEFINE_METHOD,
    INVOKE,       // operands: name-constant-index (1 byte), arg-count (1 byte)
    INHERIT,      // no operand — copies superclass methods into subclass
    GET_SUPER,    // 1-byte constant (method name) — binds method to 'this' from superclass
    SUPER_INVOKE, // operands: name-constant-index (1 byte), arg-count (1 byte)
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
    void patch(int offset, Byte byte);
    std::optional<uint8_t> addConstant(Value value);
    Value getConstant(int idx) const;
    const ValueArray& constants() const { return m_constants; }
    int getLine(int offset) const;

  private:
    ValueArray m_constants;
    std::vector<std::pair<int, int>> m_lines;
};
