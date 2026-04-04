#include "debug.h"
#include "allocator.h"
#include "value.h"

#include <ostream>

static int simpleInstruction(const char* name, int offset, std::ostream& out) {
    out << name << '\n';
    return offset + 1;
}

static int constantInstruction(const char* name, const Chunk& chunk,
                               const Allocator& alloc, int offset,
                               std::ostream& out) {
    uint8_t idx = chunk.at(offset + 1);
    out << name << ' ' << static_cast<int>(idx) << " ('"
        << stringify(chunk.getConstant(idx), alloc) << "')\n";
    return offset + 2;
}

void disassembleChunk(const Chunk& chunk, const Allocator& alloc,
                      const char* name, std::ostream& out) {
    out << "== " << name << " ==\n";
    for (int offset = 0; offset < static_cast<int>(chunk.size());) {
        offset = disassembleInstruction(chunk, alloc, offset, out);
    }
}

int disassembleInstruction(const Chunk& chunk, const Allocator& alloc,
                           int offset, std::ostream& out) {
    out << offset << ": ";

    Op instruction = toOpcode(chunk.at(offset));
    switch (instruction) {
    case Op::CONSTANT:
        return constantInstruction("CONSTANT", chunk, alloc, offset, out);
    case Op::NIL:
        return simpleInstruction("NIL", offset, out);
    case Op::TRUE:
        return simpleInstruction("TRUE", offset, out);
    case Op::FALSE:
        return simpleInstruction("FALSE", offset, out);
    case Op::EQUAL:
        return simpleInstruction("EQUAL", offset, out);
    case Op::GREATER:
        return simpleInstruction("GREATER", offset, out);
    case Op::LESS:
        return simpleInstruction("LESS", offset, out);
    case Op::NEGATE:
        return simpleInstruction("NEGATE", offset, out);
    case Op::ADD:
        return simpleInstruction("ADD", offset, out);
    case Op::SUBTRACT:
        return simpleInstruction("SUBTRACT", offset, out);
    case Op::MULTIPLY:
        return simpleInstruction("MULTIPLY", offset, out);
    case Op::DIVIDE:
        return simpleInstruction("DIVIDE", offset, out);
    case Op::NOT:
        return simpleInstruction("NOT", offset, out);
    case Op::PRINT:
        return simpleInstruction("PRINT", offset, out);
    case Op::POP:
        return simpleInstruction("POP", offset, out);
    case Op::RETURN:
        return simpleInstruction("RETURN", offset, out);
    default:
        out << "UNKNOWN(" << static_cast<unsigned>(chunk.at(offset)) << ")\n";
        return offset + 1;
    }
}
