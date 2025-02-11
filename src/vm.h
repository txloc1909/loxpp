#pragma once

#include "chunk.h"

enum class InterpretResult {
    OK,
    COMPILE_ERROR,
    RUNTIME_ERROR,
};

class VM {
  public:
    static constexpr int STACK_MAX = 256;

    VM() { resetStack(); }
    ~VM() {}

    InterpretResult interpret(Chunk const* chunk);
    InterpretResult run();

  private:
    Byte readByte();
    Value readConstant();
    void resetStack();
    void push(Value value);
    Value pop();

    Chunk const* m_chunk;
    Chunk::const_iterator m_ip;
    Value stack[STACK_MAX];
    Value* stackTop;
};
