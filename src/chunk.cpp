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

void Chunk::write(Byte byte, int line) {
    push_back(byte);

    if (m_lines.empty() || m_lines.back().first != line) {
        m_lines.push_back({line, 1});
    } else {
        m_lines.back().second++;
    }
}

void Chunk::write(Op op, int line) { write(static_cast<Byte>(op), line); }

uint8_t Chunk::addConstant(Value value) {
    m_constants.write(value);
    return m_constants.size() - 1;
}

int Chunk::getLine(int offset) const {
    int currentOffset = 0;
    for (const auto& [line, count] : m_lines) {
        currentOffset += count;
        if (offset < currentOffset) {
            return line;
        }
    }
    return 0; // Error case
}

int Chunk::disassembleInstruction(int offset) const {
    std::printf("%04d ", offset);

    if (offset > 0 && getLine(offset) == getLine(offset - 1)) {
        std::printf("   | ");
    } else {
        std::printf("%4d ", getLine(offset));
    }

    Op instruction = toOpcode(this->at(offset));
    switch (instruction) {
    case Op::RETURN:
        return simpleInstruction("Op::RETURN", offset);
    case Op::NEGATE:
        return simpleInstruction("Op::NEGATE", offset);
    case Op::CONSTANT:
        return constantInstruction("Op::CONSTANT", *this, offset);
    case Op::ADD:
        return simpleInstruction("Op::ADD", offset);
    case Op::SUBTRACT:
        return simpleInstruction("Op::SUBTRACT", offset);
    case Op::MULTIPLY:
        return simpleInstruction("Op::MULTIPLY", offset);
    case Op::DIVIDE:
        return simpleInstruction("Op::DIVIDE", offset);
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
