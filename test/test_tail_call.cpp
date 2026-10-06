// test_tail_call.cpp — self tail-call elimination (spec/04-semantics.md,
// "Self Tail Calls"). A `return f(args);` inside global function `f` runs
// without growing the call stack, except where a bail-out keeps the plain call.

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
        // a class declaration in the body
        "fun f(n) { class C { m() { return n; } } return f(n + 1); }",
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

// A function declared in a block is not a global, so it keeps the plain call.
TEST_F(TailCallTest, BlockScopedFunctionKeepsStackGrowth) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var kind;
        {
            fun f(n) { return f(n + 1); }
            try { f(0); } catch (e) { kind = e.kind; }
        }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "StackOverflowError");
}

// The global is rebound to a wrapper while the original runs: the tail call
// must reach the wrapper, as an ordinary call would.
TEST_F(TailCallTest, ReboundGlobalTakesTheOrdinaryCall) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var traces = 0;
        fun count(n) {
            if (n == 0) return "base";
            return count(n - 1);
        }
        var orig = count;
        fun count(n) {
            traces = traces + 1;
            return orig(n);
        }
        var result = count(3);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "base");
    EXPECT_DOUBLE_EQ(as<Number>(*h.getGlobal("traces")), 4.0);
}

TEST_F(TailCallTest, GlobalSetToNilRaisesNotCallable) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f(n) {
            if (n == 0) return "f-base";
            return f(n - 1);
        }
        var g = f;
        f = nil;
        var kind;
        try { g(3); } catch (e) { kind = e.kind; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("kind"), "NotCallableError");
}

// Only an assignment to the bare name matters; a property of the same name
// and a closed inner-scope variable of the same name leave the call a self
// tail call.
TEST_F(TailCallTest, SameNameProperty_AndClosedInnerScope_StillTailCall) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box {}
        var box = Box();
        fun f(n, total) {
            box.f = n;
            { var f = 1; total = total + f; }
            if (n == 0) return total;
            return f(n - 1, total);
        }
        var result = f(100000, 0);
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(*h.getGlobal("result")), 100001.0);
}

// A coroutine body that suspends between self tail calls keeps working and
// runs in constant call depth.
TEST_F(TailCallTest, TailRecursiveCoroutineBody) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun gen(n) {
            if (n == 0) return "done";
            yield n;
            return gen(n - 1);
        }
        var co = coroutine.create(gen);
        var first = co.resume(100000);
        var second = co.resume();
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(*h.getGlobal("first")), 100000.0);
    EXPECT_DOUBLE_EQ(as<Number>(*h.getGlobal("second")), 99999.0);
}
