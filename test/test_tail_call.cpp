// test_tail_call.cpp — self tail-call elimination (spec/04-semantics.md,
// "Tail calls"). A `return f(args);` inside global function `f` runs without
// growing the call stack, except where a bail-out keeps the plain call.

#include "test_harness.h"
#include <gtest/gtest.h>

class TailCallTest : public ::testing::Test {};

TEST_F(TailCallTest, DeepSelfTailRecursionFinishes) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun count(n, acc) {
            if (n == 0) return acc;
            return count(n - 1, acc + 1);
        }
        var result = count(1000000, 0);
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(*h.getGlobal("result")), 1000000.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(TailCallTest, NonTailRecursionStillOverflows) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun count(n) {
            if (n == 0) return 0;
            return 1 + count(n - 1);
        }
        var kind;
        try { count(1000000); } catch (e) { kind = e.kind; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "StackOverflowError");
}

// Arguments are all evaluated before any parameter slot is overwritten.
TEST_F(TailCallTest, ArgumentsReadOldParameters) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun fib(n, a, b) {
            if (n == 0) return a;
            return fib(n - 1, b, a + b);
        }
        var result = fib(30, 0, 1);
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(*h.getGlobal("result")), 832040.0);
}

// Locals and loop state live above the parameters; the loop head must see
// the same stack height as function entry.
TEST_F(TailCallTest, LocalsAndLoopsAboveParametersAreDropped) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f(n, total) {
            var step = 2;
            for (var i = 0; i < 3; i = i + 1) {
                if (i == 1) {
                    var inner = i * step;
                    if (n == 0) return total;
                    return f(n - 1, total + inner);
                }
            }
            return -1;
        }
        var result = f(100000, 0);
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(*h.getGlobal("result")), 200000.0);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(TailCallTest, ZeroArityAndArityMismatch) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var n = 100000;
        fun tick() {
            if (n == 0) return "done";
            n = n - 1;
            return tick();
        }
        var result = tick();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "done");

    // Wrong argument count keeps the ordinary arity error.
    VMTestHarness h2;
    EXPECT_EQ(h2.run("fun f(a) { return f(); } f(1);"),
              InterpretResult::RUNTIME_ERROR);
}

// A parameter that shadows the function name is not a self call.
TEST_F(TailCallTest, ShadowedNameIsNotSelfCall) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f(f) { return f(1); }
        fun g(x) { return x + 1; }
        var result = f(g);
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(*h.getGlobal("result")), 2.0);
}

// Bail-outs keep the plain call, so deep recursion still overflows.
TEST_F(TailCallTest, BailOutsKeepStackGrowth) {
    const char* bodies[] = {
        // closure in the body
        "fun f(n) { fun g() { return n; } return f(n + 1); }",
        // try in the body
        "fun f(n) { try { n = n; } catch (e) { } return f(n + 1); }",
        // defer in the body
        "fun noop() { } fun f(n) { defer noop(); return f(n + 1); }",
        // the function rebinds its own name
        "fun f(n) { var keep = f; f = keep; return f(n + 1); }",
    };
    for (const char* body : bodies) {
        VMTestHarness h;
        std::string src =
            std::string(body) +
            "var kind; try { f(0); } catch (e) { kind = e.kind; }";
        ASSERT_EQ(h.run(src), InterpretResult::OK) << body;
        EXPECT_EQ(h.getGlobalStr("kind"), "StackOverflowError") << body;
    }
}

// The call must be the whole return expression.
TEST_F(TailCallTest, CallInsideLargerExpressionIsNotTail) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f(n) { if (n == 0) return 0; return f(n - 1) + 1; }
        var kind;
        try { f(1000000); } catch (e) { kind = e.kind; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "StackOverflowError");
}
