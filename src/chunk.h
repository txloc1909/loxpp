#pragma once

#include <stdint.h>
#include <vector>

using Byte = uint8_t;

enum class Op : Byte {
    RETURN,
};

inline Op toOpcode(Byte byte) { return static_cast<Op>(byte); }

class Chunk : std::vector<Byte> {
  public:
    void write(Byte byte);
    void write(Op op);
    void disassemble(const char* name);
    int disassembleInstruction(int offset);
};
