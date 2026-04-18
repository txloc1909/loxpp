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

#include "test_harness.h"
#include "vm.h"
#include <gtest/gtest.h>

#ifdef LOXPP_PROFILE
#include "chunk.h"
#include "profiler.h"
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

#else // LOXPP_PROFILE not defined

// Placeholder so the test binary compiles and reports a clear skip message.
TEST_F(ProfilerTest, SkippedWhenProfilingDisabled) {
    GTEST_SKIP() << "Profiler tests require LOXPP_PROFILE compile flag";
}

#endif // LOXPP_PROFILE
