#pragma once

#include "chunk.h"
#include "object.h"

#include <memory>
#include <string>
#include <vector>

enum class InterpretResult {
    OK,
    COMPILE_ERROR,
    RUNTIME_ERROR,
};

class VM {
  public:
    static constexpr int STACK_MAX = 256;

    VM() { resetStack(); }

    InterpretResult interpret(const std::string& source);
    InterpretResult run();

  private:
    Byte readByte();
    Value readConstant();
    void resetStack();
    void push(Value value);
    Value pop();
    Value peek(int distance);

    void runtimeError(const char* format, ...);

    Chunk const* m_chunk;
    Chunk::const_iterator m_ip;
    Value stack[STACK_MAX];
    Value* stackTop;
    std::vector<std::unique_ptr<Obj>> m_objects;
};