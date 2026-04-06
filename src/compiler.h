#pragma once

#include "chunk.h"
#include "parser.h"

#include <memory>
#include <string>

class MemoryManager;

static constexpr int UINT8_COUNT = 256;

std::unique_ptr<Chunk> compile(const std::string& source, MemoryManager* mm);

struct Local {
    Token name;
    int depth; // -1 = declared but not yet initialized; ≥0 = scope depth
};

class Compiler {
  public:
    Compiler(Chunk* chunk, Parser* parser, MemoryManager* mm);

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
    void ifStatement();
    void whileStatement();
    void forStatement();
    void varDeclaration();
    void block();

    void and_();
    void or_();

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
    int emitJump(Op op);
    void patchJump(int offset);
    void emitLoop(int loopStart);

    uint8_t makeConstant(Value value);
    uint8_t identifierConstant(const Token& name);
    void namedVariable(const Token& name, bool canAssign);

    Chunk* m_currentChunk;
    Parser* m_parser;
    MemoryManager* m_mm;

    Local m_locals[UINT8_COUNT];
    int m_localCount{0};
    int m_scopeDepth{0};
};
