#pragma once

#include "value.h"

#include <stdint.h>
#include <vector>
#include <utility>

using Byte = uint8_t;

enum class Op : Byte {
    CONSTANT,
    NEGATE,
    ADD,
    SUBTRACT,
    MULTIPLY,
    DIVIDE,
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

    void write(Byte byte, int line);
    void write(Op op, int line);
    int addConstant(Value value);
    void disassemble(const char* name) const;
    int disassembleInstruction(int offset) const;
    Value getConstant(int idx) const;
    int getLine(int offset) const;

  private:
    ValueArray m_constants;
    std::vector<std::pair<int, int>> m_lines;
};
