#include "debug.h"
#include "function.h"
#include "memory_manager.h"
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
                               const MemoryManager& mm, int offset,
                               std::ostream& out, bool color) {
    uint8_t idx = chunk.at(offset + 1);
    out << cc(color, kBold) << name << cc(color, kReset) << ' '
        << static_cast<int>(idx) << " ('" << cc(color, kYellow)
        << stringify(chunk.getConstant(idx)) << cc(color, kReset) << "')\n";
    return offset + 2;
}

static int byteInstruction(const char* name, const Chunk& chunk, int offset,
                           std::ostream& out, bool color) {
    uint8_t slot = chunk.at(offset + 1);
    out << cc(color, kBold) << name << cc(color, kReset) << ' '
        << static_cast<int>(slot) << '\n';
    return offset + 2;
}

static int jumpInstruction(const char* name, int sign, const Chunk& chunk,
                           int offset, std::ostream& out, bool color) {
    uint16_t jump =
        static_cast<uint16_t>(chunk.at(offset + 1) << 8 | chunk.at(offset + 2));
    int target = offset + 3 + sign * static_cast<int>(jump);
    out << cc(color, kBold) << name << cc(color, kReset) << ' ' << offset
        << " -> " << target << '\n';
    return offset + 3;
}

static int invokeInstruction(const char* name, const Chunk& chunk,
                             const MemoryManager& mm, int offset,
                             std::ostream& out, bool color) {
    uint8_t nameIdx = chunk.at(offset + 1);
    uint8_t argCount = chunk.at(offset + 2);
    out << cc(color, kBold) << name << cc(color, kReset) << ' '
        << static_cast<int>(nameIdx) << " ('" << cc(color, kYellow)
        << stringify(chunk.getConstant(nameIdx)) << cc(color, kReset) << "') "
        << static_cast<int>(argCount) << '\n';
    return offset + 3;
}

static int closureInstruction(const char* name, const Chunk& chunk, int offset,
                              std::ostream& out, bool color) {
    uint8_t idx = chunk.at(offset + 1);
    Value fnVal = chunk.getConstant(idx);
    ObjFunction* fn = asObjFunction(fnVal);
    out << cc(color, kBold) << name << cc(color, kReset) << ' '
        << static_cast<int>(idx) << " ('" << cc(color, kYellow)
        << stringify(fnVal) << cc(color, kReset) << "')\n";
    offset += 2;
    for (int i = 0; i < fn->upvalueCount; i++) {
        uint8_t isLocal = chunk.at(offset++);
        uint8_t index = chunk.at(offset++);
        out << cc(color, kDim) << "           |  "
            << (isLocal != 0U ? "local" : "upvalue") << ' '
            << static_cast<int>(index) << cc(color, kReset) << '\n';
    }
    return offset;
}

void disassembleChunk(const Chunk& chunk, const MemoryManager& mm,
                      const char* name, std::ostream& out, bool color) {
    out << cc(color, kBold) << "== " << name << " ==" << cc(color, kReset)
        << '\n';
    for (int offset = 0; offset < static_cast<int>(chunk.size());) {
        offset = disassembleInstruction(chunk, mm, offset, out, color);
    }
}

int disassembleInstruction(const Chunk& chunk, const MemoryManager& mm,
                           int offset, std::ostream& out, bool color) {
    out << cc(color, kDim) << offset << ": " << cc(color, kReset);

    Op instruction = toOpcode(chunk.at(offset));
    switch (instruction) {
    case Op::CONSTANT:
        return constantInstruction("CONSTANT", chunk, mm, offset, out, color);
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
    case Op::MODULO:
        return simpleInstruction("MODULO", offset, out, color);
    case Op::NOT:
        return simpleInstruction("NOT", offset, out, color);
    case Op::PRINT:
        return simpleInstruction("PRINT", offset, out, color);
    case Op::POP:
        return simpleInstruction("POP", offset, out, color);
    case Op::GET_LOCAL:
        return byteInstruction("GET_LOCAL", chunk, offset, out, color);
    case Op::SET_LOCAL:
        return byteInstruction("SET_LOCAL", chunk, offset, out, color);
    case Op::DEFINE_GLOBAL:
        return constantInstruction("DEFINE_GLOBAL", chunk, mm, offset, out,
                                   color);
    case Op::GET_GLOBAL:
        return constantInstruction("GET_GLOBAL", chunk, mm, offset, out, color);
    case Op::SET_GLOBAL:
        return constantInstruction("SET_GLOBAL", chunk, mm, offset, out, color);
    case Op::JUMP:
        return jumpInstruction("JUMP", 1, chunk, offset, out, color);
    case Op::JUMP_IF_FALSE:
        return jumpInstruction("JUMP_IF_FALSE", 1, chunk, offset, out, color);
    case Op::LOOP:
        return jumpInstruction("LOOP", -1, chunk, offset, out, color);
    case Op::CALL:
        return byteInstruction("CALL", chunk, offset, out, color);
    case Op::RETURN:
        return simpleInstruction("RETURN", offset, out, color);
    case Op::CLOSURE:
        return closureInstruction("CLOSURE", chunk, offset, out, color);
    case Op::GET_UPVALUE:
        return byteInstruction("GET_UPVALUE", chunk, offset, out, color);
    case Op::SET_UPVALUE:
        return byteInstruction("SET_UPVALUE", chunk, offset, out, color);
    case Op::CLOSE_UPVALUE:
        return simpleInstruction("CLOSE_UPVALUE", offset, out, color);
    case Op::CLASS:
        return constantInstruction("CLASS", chunk, mm, offset, out, color);
    case Op::GET_PROPERTY:
        return constantInstruction("GET_PROPERTY", chunk, mm, offset, out,
                                   color);
    case Op::SET_PROPERTY:
        return constantInstruction("SET_PROPERTY", chunk, mm, offset, out,
                                   color);
    case Op::DEFINE_METHOD:
        return constantInstruction("DEFINE_METHOD", chunk, mm, offset, out,
                                   color);
    case Op::INVOKE:
        return invokeInstruction("INVOKE", chunk, mm, offset, out, color);
    case Op::INHERIT:
        return simpleInstruction("INHERIT", offset, out, color);
    case Op::GET_SUPER:
        return constantInstruction("GET_SUPER", chunk, mm, offset, out, color);
    case Op::SUPER_INVOKE:
        return invokeInstruction("SUPER_INVOKE", chunk, mm, offset, out, color);
    case Op::BUILD_LIST:
        return byteInstruction("BUILD_LIST", chunk, offset, out, color);
    case Op::GET_INDEX:
        return simpleInstruction("GET_INDEX", offset, out, color);
    case Op::SET_INDEX:
        return simpleInstruction("SET_INDEX", offset, out, color);
    case Op::IN:
        return simpleInstruction("IN", offset, out, color);
    default:
        out << cc(color, kRed) << cc(color, kBold) << "UNKNOWN("
            << static_cast<unsigned>(chunk.at(offset)) << ")"
            << cc(color, kReset) << '\n';
        return offset + 1;
    }
}
