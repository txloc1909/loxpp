#include "chunk.h"

#include <cstdio>

static int simpleInstruction(const char* name, int offset) {
    std::printf("%s\n", name);
    return offset + 1;
}

static int constantInstruction(const char* name, const Chunk& chunk,
                               int offset) {
    auto constantIdx = chunk.at(offset + 1);
    std::printf("%-16s %4d '%g'\n", name, constantIdx,
                chunk.getConstant(constantIdx));
    return offset + 2;
}

void Chunk::write(Byte byte) { push_back(byte); }

void Chunk::write(Op op) { write(static_cast<Byte>(op)); }

int Chunk::addConstant(Value value) {
    m_constants.write(value);
    return m_constants.size() - 1;
}

int Chunk::disassembleInstruction(int offset) const {
    std::printf("%04d ", offset);

    Op instruction = toOpcode(this->at(offset));
    switch (instruction) {
    case Op::RETURN:
        return simpleInstruction("Op::RETURN", offset);
    case Op::CONSTANT:
        return constantInstruction("Op::CONSTANT", *this, offset);
    default:
        std::printf("Unknown opcode %d\n", instruction);
        return offset + 1;
    }
}

void Chunk::disassemble(const char* name) const {
    std::printf("== %s ==\n", name);

    for (int offset = 0; offset < size();) {
        offset = disassembleInstruction(offset);
    }
}

Value Chunk::getConstant(int idx) const { return m_constants.at(idx); }
