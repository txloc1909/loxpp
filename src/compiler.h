#pragma once

#include "chunk.h"
#include "memory.h"
#include "parser.h"

#include <memory>
#include <string>
#include <vector>

struct Table;

std::unique_ptr<Chunk> compile(const std::string& source,
                               std::vector<std::unique_ptr<Obj>>& objects,
                               Table* strings = nullptr);

class Compiler {
  public:
    Compiler(Chunk* chunk, Parser* parser,
             std::vector<std::unique_ptr<Obj>>* objects,
             Table* strings = nullptr);

    Chunk* getCurrentChunk() const { return m_currentChunk; }
    void endCompiler();

    void expression();
    void grouping();
    void unary();
    void binary();
    void literal();
    void number();
    void string();

  private:
    void emitByte(Byte byte);
    void emitByte(Op op);
    void emitBytes(Op op, Byte byte);
    void emitReturn();

    uint8_t makeConstant(Value value);

    Chunk* m_currentChunk;
    Parser* m_parser;
    std::vector<std::unique_ptr<Obj>>* m_objects;
    Table* m_strings;
};
