#pragma once

#include "allocator.h"
#include "chunk.h"
#include "parser.h"

#include <memory>
#include <string>

std::unique_ptr<Chunk> compile(const std::string& source, Allocator* alloc);

class Compiler {
  public:
    Compiler(Chunk* chunk, Parser* parser, Allocator* alloc);

    Chunk* getCurrentChunk() const { return m_currentChunk; }
    void endCompiler();

    void expression();
    void grouping();
    void unary();
    void binary();
    void literal();
    void number();
    void string();
    void variable();

    void declaration();
    void statement();
    void printStatement();
    void expressionStatement();
    void varDeclaration();

  private:
    void emitByte(Byte byte);
    void emitByte(Op op);
    void emitBytes(Op op, Byte byte);
    void emitReturn();

    uint8_t makeConstant(Value value);
    uint8_t identifierConstant(const Token& name);
    void namedVariable(const Token& name, bool canAssign);

    Chunk* m_currentChunk;
    Parser* m_parser;
    Allocator* m_allocator;
};
