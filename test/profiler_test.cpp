// profiler_test.cpp — runtime profiler correctness tests.
//
// All tests are compiled only when LOXPP_PROFILE is defined. Without the flag
// the test executable is built but contains no test cases (empty suite).
//
// Invariants under test:
//   1. Call count for fibRecursive(10) == 177 (analytically exact).
//   2. Op::CALL count == Op::RETURN count (balanced for any program).
//   3. selfNs <= totalNs for every profiled function.
//   4. GC stats are populated after an allocation-heavy program.
//   5. Call chain deeper than 64 frames stays in bounds (issue #299).
//   6. A coroutine's function and opcode counts reach the root report, while
//      it is suspended, completed, or abandoned (issue #538).

#include "test_harness.h"
#include "vm.h"
#include <gtest/gtest.h>

#ifdef LOXPP_PROFILE
#include "chunk.h"
#include "profiler.h"
#include <cstdlib>
#include <string>
#endif

class ProfilerTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// Helper: run source through a fresh VM and return its ProfilerData.
// Only meaningful when LOXPP_PROFILE is defined.
// ---------------------------------------------------------------------------

#ifdef LOXPP_PROFILE

static const ProfilerData& runAndGetProfile(VM& vm, const std::string& source) {
    vm.interpret(source);
    return vm.profilerData();
}

static const FunctionStats* findFunction(const ProfilerData& data,
                                         const std::string& name) {
    for (const auto& [fn, stats] : data.funcTable) {
        if (stats.name == name)
            return &stats;
    }
    return nullptr;
}

// Sets an environment variable for the enclosing scope and restores its prior
// value on exit, including on an ASSERT failure that returns early. The
// MemoryManager reads LOXPP_STRESS_GC once per VM, so a leaked value would
// change how every later test in this binary allocates.
class ScopedEnvVar {
  public:
    ScopedEnvVar(const char* name, const char* value) : m_name(name) {
        const char* prev = std::getenv(name);
        m_hadPrev = prev != nullptr;
        if (m_hadPrev)
            m_prev = prev;
        ::setenv(name, value, 1);
    }

    ~ScopedEnvVar() {
        if (m_hadPrev)
            ::setenv(m_name.c_str(), m_prev.c_str(), 1);
        else
            ::unsetenv(m_name.c_str());
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

  private:
    std::string m_name;
    std::string m_prev;
    bool m_hadPrev{false};
};

// ---------------------------------------------------------------------------
// 1. fibRecursive(10) must record exactly 177 calls.
//
// The exponential Fibonacci recursion tree for fib(10) visits nodes in a
// pattern where the total call count equals fib(12) - 1 = 177. This is an
// analytically exact invariant — any miscount indicates a missed entry/exit.
// ---------------------------------------------------------------------------

TEST_F(ProfilerTest, FibRecursiveCallCount) {
    VM vm;
    const ProfilerData& data = runAndGetProfile(vm, R"(
        fun fib(n) {
            if (n <= 1) return n;
            return fib(n - 1) + fib(n - 2);
        }
        fib(10);
    )");

    // Find the ObjFunction* for "fib".
    const FunctionStats* fibStats = nullptr;
    for (const auto& [fn, stats] : data.funcTable) {
        if (stats.name == "fib") {
            fibStats = &stats;
            break;
        }
    }
    ASSERT_NE(fibStats, nullptr) << "No profiler entry for 'fib'";
    EXPECT_EQ(fibStats->callCount, 177u)
        << "fibRecursive(10) must make exactly 177 recursive calls";
}

// ---------------------------------------------------------------------------
// 2. Op::CALL count must equal Op::RETURN count for any well-formed program.
// ---------------------------------------------------------------------------

TEST_F(ProfilerTest, CallReturnBalance) {
    VM vm;
    const ProfilerData& data = runAndGetProfile(vm, R"(
        fun hanoi(n, from, to, via) {
            if (n == 0) return;
            hanoi(n - 1, from, via, to);
            hanoi(n - 1, via, to, from);
        }
        hanoi(6, "A", "C", "B");
    )");

    uint64_t callCount = data.opcodeTable[static_cast<uint8_t>(Op::CALL)].count;
    uint64_t returnCount =
        data.opcodeTable[static_cast<uint8_t>(Op::RETURN)].count;
    EXPECT_EQ(callCount, returnCount)
        << "Op::CALL and Op::RETURN counts must be balanced";
}

// ---------------------------------------------------------------------------
// 3. selfNs <= totalNs for every profiled function.
// ---------------------------------------------------------------------------

TEST_F(ProfilerTest, SelfTimeLeTotalTime) {
    VM vm;
    const ProfilerData& data = runAndGetProfile(vm, R"(
        fun inner(n) { var x = n * 2; return x; }
        fun outer(n) {
            var i = 0;
            while (i < n) {
                inner(i);
                i = i + 1;
            }
        }
        outer(100);
    )");

    for (const auto& [fn, stats] : data.funcTable) {
        EXPECT_LE(stats.selfNs, stats.totalNs)
            << "selfNs must be <= totalNs for function: " << stats.name;
    }
}

// ---------------------------------------------------------------------------
// 4. GC stats are populated after an allocation-heavy program.
// ---------------------------------------------------------------------------

TEST_F(ProfilerTest, GcStatsPopulated) {
    VM vm;
    // Build many short-lived strings to trigger at least one GC cycle.
    const ProfilerData& data = runAndGetProfile(vm, R"(
        fun allocLoop() {
            var i = 0;
            while (i < 20000) {
                var s = str(i);
                i = i + 1;
            }
        }
        allocLoop();
    )");

    EXPECT_GT(data.gc.gcCount, uint64_t{0})
        << "Expected at least one GC collection from allocation-heavy program";
    EXPECT_GT(data.gc.totalBytesFreed, std::size_t{0})
        << "Expected non-zero bytes freed after GC";
}

// ---------------------------------------------------------------------------
// 5. Call chain deeper than 64 frames stays in bounds (issue #299).
//
// frameEnterNs runs parallel to m_frames[]. Depth 200 exceeds the old
// hardcoded size (64) but stays well below FRAMES_MAX, so it fails
// before the fix and passes after it.
// ---------------------------------------------------------------------------

TEST_F(ProfilerTest, DeepRecursionPast64Frames) {
    VM vm;
    InterpretResult result = vm.interpret(R"(
        fun recurse(n) {
            if (n <= 0) return 0;
            recurse(n - 1);
            return 0;
        }
        recurse(200);
    )");
    ASSERT_EQ(result, InterpretResult::OK) << "200-deep chain must run clean";
    const ProfilerData& data = vm.profilerData();

    const FunctionStats* stats = nullptr;
    for (const auto& [fn, s] : data.funcTable) {
        if (s.name == "recurse") {
            stats = &s;
            break;
        }
    }
    ASSERT_NE(stats, nullptr) << "No profiler entry for 'recurse'";
    EXPECT_EQ(stats->callCount, 201u) << "recurse(200) must record 201 calls";
}

// ---------------------------------------------------------------------------
// 6. A coroutine that suspends and resumes repeatedly must not corrupt the
//    per-coroutine profiler scope slice. Its functions are profiled under the
//    coroutine's own ProfilerData, so the root report only needs to prove the
//    shared scope array survives each hand-off (issue #526).
// ---------------------------------------------------------------------------
TEST_F(ProfilerTest, CoroutineSuspendResumeKeepsProfileConsistent) {
    VM vm;
    InterpretResult result = vm.interpret(R"(
        fun gen() {
            var i = 0;
            while (i < 3) {
                yield i;
                i = i + 1;
            }
            return i;
        }
        var co = coroutine.create(gen);
        co.resume();
        co.resume();
        co.resume();
    )");
    ASSERT_EQ(result, InterpretResult::OK);

    const ProfilerData& data = vm.profilerData();
    bool sawScript = false;
    for (const auto& [fn, s] : data.funcTable) {
        if (s.name == "<script>") {
            sawScript = true;
            EXPECT_LE(s.selfNs, s.totalNs);
        }
    }
    EXPECT_TRUE(sawScript) << "root profiler must still profile the script";
}

// ---------------------------------------------------------------------------
// 7. A coroutine's function and opcode counts must reach the root report
//    (issue #538). The coroutine is suspended when the program ends, so this
//    exercises the report-time merge of a live coroutine.
// ---------------------------------------------------------------------------
TEST_F(ProfilerTest, SuspendedCoroutineFunctionMergedIntoReport) {
    VM vm;
    InterpretResult result = vm.interpret(R"(
        fun gen() {
            yield 1;
        }
        var co = coroutine.create(gen);
        co.resume();
    )");
    ASSERT_EQ(result, InterpretResult::OK);

    const ProfilerData& data = vm.profilerData();
    const FunctionStats* gen = findFunction(data, "gen");
    ASSERT_NE(gen, nullptr)
        << "suspended coroutine's function must appear in the report";
    EXPECT_EQ(gen->callCount, 1u);
    EXPECT_GT(data.opcodeTable[static_cast<uint8_t>(Op::YIELD)].count, 0u)
        << "YIELD runs only inside the coroutine and must be merged";
}

// ---------------------------------------------------------------------------
// 8. A coroutine that runs to completion is dead but still referenced when the
//    report prints, so its function must still be merged (issue #538).
// ---------------------------------------------------------------------------
TEST_F(ProfilerTest, CompletedCoroutineFunctionMergedIntoReport) {
    VM vm;
    InterpretResult result = vm.interpret(R"(
        fun worker() { return 42; }
        var co = coroutine.create(worker);
        co.resume();
    )");
    ASSERT_EQ(result, InterpretResult::OK);

    const ProfilerData& data = vm.profilerData();
    const FunctionStats* worker = findFunction(data, "worker");
    ASSERT_NE(worker, nullptr)
        << "completed coroutine's function must appear in the report";
    EXPECT_EQ(worker->callCount, 1u);
}

// ---------------------------------------------------------------------------
// 9. An abandoned coroutine is collected by the sweeper, which must fold its
//    profile into the root before the object (and its profiler) is freed
//    (issue #538). LOXPP_STRESS_GC forces the sweep within the program.
// ---------------------------------------------------------------------------
TEST_F(ProfilerTest, AbandonedCoroutineFunctionMergedOnSweep) {
    ScopedEnvVar stressGC("LOXPP_STRESS_GC", "1");
    VM vm;
    InterpretResult result = vm.interpret(R"(
        fun gen() {
            yield 1;
        }
        var co = coroutine.create(gen);
        co.resume();
        co = nil;
        var i = 0;
        while (i < 50) { var s = str(i); i = i + 1; }
    )");
    ASSERT_EQ(result, InterpretResult::OK);

    const ProfilerData& data = vm.profilerData();
    EXPECT_NE(findFunction(data, "gen"), nullptr)
        << "abandoned coroutine's function must survive its sweep";
}

// ---------------------------------------------------------------------------
// 10. A coroutine suspended across a report must keep a usable profiler. The
//     merge clears its tables but must not free or move the ProfilerData that
//     its live scopes still point at, or the next resume reads freed memory
//     (issue #538). Two interpret() calls are the REPL session shape.
// ---------------------------------------------------------------------------
TEST_F(ProfilerTest, ResumeAfterReportKeepsProfilerConsistent) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        fun gen() {
            yield 1;
            yield 2;
        }
        var co = coroutine.create(gen);
        co.resume();
    )"),
              InterpretResult::OK);
    ASSERT_EQ(vm.interpret("co.resume();"), InterpretResult::OK);

    const ProfilerData& data = vm.profilerData();
    const FunctionStats* gen = findFunction(data, "gen");
    ASSERT_NE(gen, nullptr)
        << "a coroutine resumed after a report must still be profiled";
    EXPECT_EQ(gen->callCount, 1u) << "one activation spans both resumes";
    EXPECT_LE(gen->selfNs, gen->totalNs);
}

#else // LOXPP_PROFILE not defined

// Placeholder so the test binary compiles and reports a clear skip message.
TEST_F(ProfilerTest, SkippedWhenProfilingDisabled) {
    GTEST_SKIP() << "Profiler tests require LOXPP_PROFILE compile flag";
}

#endif // LOXPP_PROFILE
