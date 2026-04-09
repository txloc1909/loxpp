#pragma once

#include "function.h"
#include "memory_manager.h"
#include "native.h"
#include "table.h"

#include <memory>
#include <optional>
#include <string>

enum class InterpretResult {
    OK,
    COMPILE_ERROR,
    RUNTIME_ERROR,
};

struct CallFrame {
    ObjClosure* closure;
    Chunk::const_iterator ip;
    Value* slots; // points into the VM stack at this frame's base slot
};

class VM {
  public:
    static constexpr int STACK_MAX = 256;
    static constexpr int FRAMES_MAX = 64;

    VM() : m_globals(VmAllocator<Entry>{&m_mm}) { resetStack(); }

    InterpretResult interpret(const std::string& source);
    InterpretResult run();
    Value lastResult() const;

    // Runtime state inspection (for testing and debugging).
    int stackDepth() const { return static_cast<int>(stackTop - stack); }
    int frameCount() const { return m_frameCount; }
    std::optional<Value> getGlobal(const std::string& name) const;

  private:
    Byte readByte();
    uint16_t readShort();
    Value readConstant();
    void resetStack();
    void push(Value value);
    Value pop();
    Value peek(int distance);

    bool call(ObjClosure* closure, int argCount);
    bool callNative(ObjNative* native, int argCount);
    void defineNative(const char* name, NativeFn fn, int arity);
    void defineNatives();
    ObjUpvalue* captureUpvalue(Value* local);
    void closeUpvalues(Value* last);
    void runtimeError(const char* format, ...);

    CallFrame m_frames[FRAMES_MAX];
    int m_frameCount{0};
    Value stack[STACK_MAX];
    Value* stackTop;
    MemoryManager m_mm;
    Table m_globals;
    ObjUpvalue* m_openUpvalues{nullptr};
    Value m_lastResult; // For testing/debugging only -- stores the value
                        // popped by Op::POP.
};
