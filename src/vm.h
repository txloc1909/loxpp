#pragma once

#include "allocator.h"
#include "chunk.h"
#include "simple_allocator.h"

#include <memory>
#include <string>

enum class InterpretResult {
    OK,
    COMPILE_ERROR,
    RUNTIME_ERROR,
};

class VM {
  public:
    static constexpr int STACK_MAX = 256;

    VM() : m_allocator{std::make_unique<SimpleAllocator>()} { resetStack(); }

    InterpretResult interpret(const std::string& source);
    InterpretResult run();
    Value lastResult() const;
    Allocator& allocator() { return *m_allocator; }

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
    std::unique_ptr<Allocator> m_allocator;
    Value m_lastResult;
};
