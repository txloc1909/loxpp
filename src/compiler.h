#pragma once

#include "chunk.h"
#include "parser.h"

#include <string>
#include <memory>

std::unique_ptr<Chunk> compile(const std::string& source);

class Compiler {
  public:
    Compiler(Chunk* chunk, Parser* parser);

    Chunk* getCurrentChunk() const { return m_currentChunk; }
    void endCompiler();

    void expression();
    void grouping();
    void unary();
    void binary();
    void literal();
    void number();

  private:
    void emitByte(Byte byte);
    void emitByte(Op op);
    void emitBytes(Op op, Byte byte);
    void emitReturn();

    uint8_t makeConstant(Value value);

    Chunk* m_currentChunk;
    Parser* m_parser;
};
