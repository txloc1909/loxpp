#pragma once

#include "function.h"
#include "memory_manager.h"
#include "native.h"
#include "table.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

enum class InterpretResult : std::uint8_t {
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
    static constexpr int STACK_MAX = 2048;
    static constexpr int FRAMES_MAX = 256;

    VM() : m_globals(VmAllocator<Entry>{&m_mm}) {
        resetStack();
        m_mm.setMarkRootsCallback([this]() { markRoots(); });
    }

    InterpretResult interpret(const std::string& source);
    InterpretResult run();
    [[nodiscard]] Value lastResult() const;

    // Runtime state inspection (for testing and debugging).
    [[nodiscard]] int stackDepth() const {
        return static_cast<int>(stackTop - stack);
    }
    [[nodiscard]] int frameCount() const { return m_frameCount; }
    [[nodiscard]] std::optional<Value> getGlobal(const std::string& name) const;

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
    bool bindMethod(ObjClass* klass, ObjString* name);
    void defineNative(const char* name, NativeFn fn, int arity);
    void defineNatives();
    void defineMathObject();
    void defineFileAPI();
    void defineMapAPI();
    void defineReflectObject();
    ObjUpvalue* captureUpvalue(Value* local);
    void closeUpvalues(Value* last);
    void runtimeError(const char* format, ...);
    void markRoots();

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
