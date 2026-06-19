#pragma once

#include "exec_objects.h"
#include "class_objects.h"
#include "memory_manager.h"
#include "table.h"
#include "stdlib/stdlib_context.h"

#include <cstdint>

#ifdef LOXPP_PROFILE
#include "profiler.h"
#endif

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
#ifdef LOXPP_PROFILE
        m_mm.setProfilerData(&m_profilerData);
#endif
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
    void defineNatives();
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
    StdlibContext m_stdlibCtx;
    ObjClass* m_fileClass{nullptr};
    ObjClass* m_mapClass{nullptr};

#ifdef LOXPP_PROFILE
    ProfilerData m_profilerData;
    // Parallel to m_frames[]: active ProfileFunctionScope per call depth.
    // .emplace() at function entry; .reset() at Op::RETURN.
    std::array<std::optional<ProfileFunctionScope>, FRAMES_MAX>
        m_profilerScopes;

  public:
    const ProfilerData& profilerData() const { return m_profilerData; }

  private:
#endif
};
