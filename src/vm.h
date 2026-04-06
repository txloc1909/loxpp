#pragma once

#include "chunk.h"
#include "memory_manager.h"
#include "table.h"

#include <memory>
#include <optional>
#include <string>

enum class InterpretResult {
    OK,
    COMPILE_ERROR,
    RUNTIME_ERROR,
};

class VM {
  public:
    static constexpr int STACK_MAX = 256;

    VM() : m_globals(VmAllocator<Entry>{&m_mm}) { resetStack(); }

    InterpretResult interpret(const std::string& source);
    InterpretResult run();
    Value lastResult() const;

    // Runtime state inspection (for testing and debugging).
    int stackDepth() const { return static_cast<int>(stackTop - stack); }
    std::optional<Value> getGlobal(const std::string& name) const;

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
    MemoryManager m_mm;
    Table m_globals;
    Value m_lastResult; // For testing/debugging only — stores the value popped
                        // by Op::POP.
};
