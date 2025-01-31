#pragma once

#include "value.h"

#include <stdint.h>
#include <vector>

using Byte = uint8_t;

enum class Op : Byte {
    CONSTANT,
    RETURN,
};

inline Op toOpcode(Byte byte) { return static_cast<Op>(byte); }

class Chunk : std::vector<Byte> {
  public:
    using std::vector<Byte>::at;

    void write(Byte byte);
    void write(Op op);
    int addConstant(Value value);
    void disassemble(const char* name) const;
    int disassembleInstruction(int offset) const;
    Value getConstant(int idx) const;

  private:
    ValueArray m_constants;
};
