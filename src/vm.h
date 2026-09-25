#pragma once

// VM — the bytecode interpreter loop. Everything it interprets against
// (the value stack, call frames, globals, GC roots, and the operations on
// all of that) lives in Runtime (runtime.h); this class caches the current
// frame/ip/chunk in local variables for dispatch speed and drives the
// switch in run(). See runtime.h's own top comment for why the two are
// split this way and what it is a step toward.

#include "runtime.h"

#include <optional>
#include <string>
#include <vector>

class VM {
  public:
    static constexpr int STACK_MAX = loxpp::kStackMax;
    static constexpr int FRAMES_MAX = loxpp::kFramesMax;
    static constexpr int STACK_OVERFLOW_FRAME_RESERVE =
        loxpp::kStackOverflowFrameReserve;
    static constexpr int STACK_OVERFLOW_STACK_RESERVE =
        loxpp::kStackOverflowStackReserve;

    VM() {
        // The one place Runtime needs the interpreter loop back — see
        // Runtime::setInterpretLoop's own comment.
        m_rt.setInterpretLoop([this](int stop) { return run(stop); });
    }

    InterpretResult interpret(const std::string& source);
    // stopAtFrameCount: returns InterpretResult::OK as soon as
    // m_rt.frameCount() drops to or below this value, instead of only at
    // program exit (frameCount == 0). Lets a deferred call run to completion
    // via a nested run() invocation — see Runtime::runPendingDefers() — before
    // its recorder resumes. The top-level call (stopAtFrameCount == 0, the
    // default) is unaffected: frameCount never reaches 0 except at program
    // exit, which the frameCount == 0 branch already handles.
    InterpretResult run(int stopAtFrameCount = 0);
    [[nodiscard]] Value lastResult() const { return m_rt.lastResult(); }

    // Runtime state inspection (for testing and debugging).
    [[nodiscard]] int stackDepth() const { return m_rt.stackDepth(); }
    [[nodiscard]] int frameCount() const { return m_rt.frameCount(); }
    [[nodiscard]] int handlerStackDepth() const {
        return m_rt.handlerStackDepth();
    }
    [[nodiscard]] std::optional<Value>
    getGlobal(const std::string& name) const {
        return m_rt.getGlobal(name);
    }

    // Test-only trace of (chunk offset, handler depth) before each
    // dispatched instruction. Null unless a test arms it. No effect on
    // release behavior: run() only appends when this is set.
    void setHandlerDepthTrace(std::vector<std::pair<int, int>>* trace) {
        m_rt.setHandlerDepthTrace(trace);
    }

    // Sets the command-line arguments exposed to the program via args().
    void setArgs(std::vector<std::string> args) {
        m_rt.setArgs(std::move(args));
    }

#ifdef LOXPP_PROFILE
    const ProfilerData& profilerData() const { return m_rt.profilerData(); }
#endif

  private:
    // White-box seam for StackOverflowTest — see Runtime's matching grant.
    friend struct VMTestAccess;

    Runtime m_rt;
};
