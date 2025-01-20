#include "chunk.h"

#include <iostream>

static int simpleInstruction(const char* name, int offset) {
    std::printf("%s\n", name);
    return offset + 1;
}

void Chunk::write(Byte byte) { push_back(byte); }

void Chunk::write(Op op) { write(static_cast<Byte>(op)); }

int Chunk::disassembleInstruction(int offset) {
    std::printf("%04d ", offset);

    Op instruction = toOpcode(this->at(offset));
    switch (instruction) {
    case Op::RETURN:
        return simpleInstruction("Op::RETURN", offset);
    default:
        std::printf("Unknown opcode %d\n", instruction);
        return offset + 1;
    }
}

void Chunk::disassemble(const char* name) {
    std::printf("== %s ==\n", name);

    for (int offset = 0; offset < size();) {
        offset = disassembleInstruction(offset);
    }
}
