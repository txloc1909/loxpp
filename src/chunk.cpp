#include "chunk.h"

void Chunk::write(Byte byte, int line) {
    push_back(byte);

    if (m_lines.empty() || m_lines.back().first != line) {
        m_lines.push_back({line, 1});
    } else {
        m_lines.back().second++;
    }
}

void Chunk::write(Op op, int line) { write(static_cast<Byte>(op), line); }

std::optional<uint8_t> Chunk::addConstant(Value value) {
    if (m_constants.isFull())
        return std::nullopt;
    m_constants.write(value);
    return static_cast<uint8_t>(m_constants.size() - 1);
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

Value Chunk::getConstant(int idx) const { return m_constants.at(idx); }
