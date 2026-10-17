#include "chunk.h"

const char* opcodeName(Op op) {
    switch (op) {
#define LOXPP_OPCODE_NAME_CASE(name)                                           \
    case Op::name:                                                             \
        return #name;
        LOXPP_FOR_EACH_OP(LOXPP_OPCODE_NAME_CASE)
#undef LOXPP_OPCODE_NAME_CASE
    }
    return "UNKNOWN";
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

void Chunk::patch(int offset, Byte byte) { (*this)[offset] = byte; }

void Chunk::truncate(std::size_t size) {
    if (size >= this->size()) {
        return;
    }
    std::size_t drop = this->size() - size;
    resize(size);
    while (drop > 0) {
        auto& run = m_lines.back();
        auto count = static_cast<std::size_t>(run.second);
        if (count > drop) {
            run.second -= static_cast<int>(drop);
            break;
        }
        drop -= count;
        m_lines.pop_back();
    }
}

std::optional<uint16_t> Chunk::addConstant(Value value) {
    // Append-only: dedup is the compiler's job (Compiler::makeConstant keeps an
    // O(1) hash index), so this stays a plain bounded push.
    if (m_constants.isFull()) {
        return std::nullopt;
    }
    m_constants.write(value);
    return static_cast<uint16_t>(m_constants.size() - 1);
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

Value Chunk::getConstant(uint16_t idx) const { return m_constants.at(idx); }
