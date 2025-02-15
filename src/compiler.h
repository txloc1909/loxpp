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

  private:
    void emitByte(Byte byte);
    void emitBytes(Op op, Byte byte);
    void emitReturn();

    Chunk* m_currentChunk;
    Parser* m_parser;
};
