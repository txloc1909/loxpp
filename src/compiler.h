#pragma once

#include "function.h"
#include "parser.h"

#include <memory>
#include <string>
#include <vector>

class MemoryManager;

static constexpr int UINT8_COUNT = 256;

ObjFunction* compile(const std::string& source, MemoryManager* mm);

struct Local {
    Token name;
    int depth; // -1 = declared but not yet initialized; >=0 = scope depth
};

struct LoopContext {
    int start;      // LOOP target: condition (while) or increment (for)
    int localCount; // m_localCount at loop entry, for cleanup POPs
    std::vector<int>
        breakJumps; // JUMP placeholder offsets to patch on loop exit
};

class Compiler {
  public:
    Compiler(ObjFunction* function, Parser* parser, MemoryManager* mm);

    Chunk* getCurrentChunk() const { return &m_function->chunk; }
    void endCompiler();

    void expression();
    void grouping();
    void unary();
    void binary();
    void literal();
    void number();
    void string();
    void variable();
    void call();

    void declaration();
    void statement();
    void printStatement();
    void expressionStatement();
    void ifStatement();
    void whileStatement();
    void forStatement();
    void breakStatement();
    void continueStatement();
    void switchStatement();
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
    void emitLoopCleanup(int targetLocalCount);

    uint8_t makeConstant(Value value);
    uint8_t identifierConstant(const Token& name);
    void namedVariable(const Token& name, bool canAssign);

    ObjFunction* m_function;
    Parser* m_parser;
    MemoryManager* m_mm;

    Local m_locals[UINT8_COUNT];
    int m_localCount{0};
    int m_scopeDepth{0};
    std::vector<LoopContext> m_loopStack;
};
