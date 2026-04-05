#pragma once

#include "allocator.h"
#include "chunk.h"
#include "parser.h"

#include <memory>
#include <string>

static constexpr int UINT8_COUNT = 256;

std::unique_ptr<Chunk> compile(const std::string& source, Allocator* alloc);

struct Local {
    Token name;
    int depth; // -1 = declared but not yet initialized; ≥0 = scope depth
};

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
    void block();

  private:
    void beginScope();
    void endScope();

    void addLocal(const Token& name);
    int resolveLocal(const Token& name) const;
    void declareVariable();
    void markInitialized();

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

    Local m_locals[UINT8_COUNT];
    int m_localCount{0};
    int m_scopeDepth{0};
};
