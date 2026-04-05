#include "debug.h"
#include "allocator.h"
#include "value.h"

#include <ostream>

namespace {

constexpr const char* kReset = "\033[0m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kRed = "\033[31m";

// Returns `code` when color is enabled, otherwise "".
inline const char* cc(bool use, const char* code) { return use ? code : ""; }

} // namespace

static int simpleInstruction(const char* name, int offset, std::ostream& out,
                             bool color) {
    out << cc(color, kBold) << name << cc(color, kReset) << '\n';
    return offset + 1;
}

static int constantInstruction(const char* name, const Chunk& chunk,
                               const Allocator& alloc, int offset,
                               std::ostream& out, bool color) {
    uint8_t idx = chunk.at(offset + 1);
    out << cc(color, kBold) << name << cc(color, kReset) << ' '
        << static_cast<int>(idx) << " ('" << cc(color, kYellow)
        << stringify(chunk.getConstant(idx), alloc) << cc(color, kReset)
        << "')\n";
    return offset + 2;
}

static int byteInstruction(const char* name, const Chunk& chunk, int offset,
                           std::ostream& out, bool color) {
    uint8_t slot = chunk.at(offset + 1);
    out << cc(color, kBold) << name << cc(color, kReset) << ' '
        << static_cast<int>(slot) << '\n';
    return offset + 2;
}

void disassembleChunk(const Chunk& chunk, const Allocator& alloc,
                      const char* name, std::ostream& out, bool color) {
    out << cc(color, kBold) << "== " << name << " ==" << cc(color, kReset)
        << '\n';
    for (int offset = 0; offset < static_cast<int>(chunk.size());) {
        offset = disassembleInstruction(chunk, alloc, offset, out, color);
    }
}

int disassembleInstruction(const Chunk& chunk, const Allocator& alloc,
                           int offset, std::ostream& out, bool color) {
    out << cc(color, kDim) << offset << ": " << cc(color, kReset);

    Op instruction = toOpcode(chunk.at(offset));
    switch (instruction) {
    case Op::CONSTANT:
        return constantInstruction("CONSTANT", chunk, alloc, offset, out,
                                   color);
    case Op::NIL:
        return simpleInstruction("NIL", offset, out, color);
    case Op::TRUE:
        return simpleInstruction("TRUE", offset, out, color);
    case Op::FALSE:
        return simpleInstruction("FALSE", offset, out, color);
    case Op::EQUAL:
        return simpleInstruction("EQUAL", offset, out, color);
    case Op::GREATER:
        return simpleInstruction("GREATER", offset, out, color);
    case Op::LESS:
        return simpleInstruction("LESS", offset, out, color);
    case Op::NEGATE:
        return simpleInstruction("NEGATE", offset, out, color);
    case Op::ADD:
        return simpleInstruction("ADD", offset, out, color);
    case Op::SUBTRACT:
        return simpleInstruction("SUBTRACT", offset, out, color);
    case Op::MULTIPLY:
        return simpleInstruction("MULTIPLY", offset, out, color);
    case Op::DIVIDE:
        return simpleInstruction("DIVIDE", offset, out, color);
    case Op::NOT:
        return simpleInstruction("NOT", offset, out, color);
    case Op::PRINT:
        return simpleInstruction("PRINT", offset, out, color);
    case Op::POP:
        return simpleInstruction("POP", offset, out, color);
    case Op::DISCARD:
        return simpleInstruction("DISCARD", offset, out, color);
    case Op::GET_LOCAL:
        return byteInstruction("GET_LOCAL", chunk, offset, out, color);
    case Op::SET_LOCAL:
        return byteInstruction("SET_LOCAL", chunk, offset, out, color);
    case Op::DEFINE_GLOBAL:
        return constantInstruction("DEFINE_GLOBAL", chunk, alloc, offset, out,
                                   color);
    case Op::GET_GLOBAL:
        return constantInstruction("GET_GLOBAL", chunk, alloc, offset, out,
                                   color);
    case Op::SET_GLOBAL:
        return constantInstruction("SET_GLOBAL", chunk, alloc, offset, out,
                                   color);
    case Op::RETURN:
        return simpleInstruction("RETURN", offset, out, color);
    default:
        out << cc(color, kRed) << cc(color, kBold) << "UNKNOWN("
            << static_cast<unsigned>(chunk.at(offset)) << ")"
            << cc(color, kReset) << '\n';
        return offset + 1;
    }
}
