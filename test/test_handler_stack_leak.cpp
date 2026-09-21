// test_handler_stack_leak.cpp — proves Op::RETURN leaves no stale
// HandlerRecord on m_handlerStack when it leaves a frame that still has an
// open try/catch region (see INVARIANT(handler-stack-frame-scoped) on
// m_handlerStack's declaration in vm.h).
//
// Invariants under test:
//   1. handlerStackDepth() is 0 after any program whose every try/catch has
//      exited, whether by normal completion, by a caught throw, or by a
//      `return` taken from inside the still-open region (body or catch),
//      at any nesting depth, inside a loop, or alongside a `defer`.
//   2. A `return` from inside an open try does not stop a later, unrelated
//      throw at the same call-frame depth from being handled correctly —
//      by its own handler, not by a stale one left behind.
//   3. `defer` registered before a `return` taken from inside an open try
//      still runs, in LIFO order, and does not itself leave a stale
//      handler record.
//   4. A defer that itself throws, when run because of a `return` out of
//      an open try, is not caught by that same try — the record it would
//      otherwise wrongly match is already gone by the time the defer
//      runs — and any earlier-registered defer still runs during the
//      resulting unwind.

#include "test_harness.h"
#include <gtest/gtest.h>

class HandlerStackLeakTest : public ::testing::Test {};

TEST_F(HandlerStackLeakTest, ReturnFromOpenTryBodyLeavesNoRecord) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun g(shouldReturn) {
            try {
                if (shouldReturn) { return "early"; }
            } catch (e) {}
        }
        var r = g(true);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("r"), "early");
}

TEST_F(HandlerStackLeakTest, ReturnFromCatchLeavesOuterTryRecordPopped) {
    VMTestHarness h;
    // The inner try/catch already pops its own record when it catches
    // "boom" (handleThrow's own bookkeeping). The outer try's record is the
    // one at risk: its POP_HANDLER is only reached on normal fall-through
    // out of the outer block, which the `return` inside the inner catch
    // skips entirely.
    ASSERT_EQ(h.run(R"(
        fun g() {
            try {
                try {
                    throw "boom";
                } catch (e) {
                    return "returned-from-catch";
                }
            } catch (outer) {
                return "unreachable";
            }
        }
        var r = g();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("r"), "returned-from-catch");
}

TEST_F(HandlerStackLeakTest, ReturnFromInnermostOfThreeNestedTriesClearsAll) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun deep() {
            try {
                try {
                    try {
                        return "deep";
                    } catch (e1) {}
                } catch (e2) {}
            } catch (e3) {}
        }
        var r = deep();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("r"), "deep");
}

TEST_F(HandlerStackLeakTest, ReturnFromTryInsideLoopAcrossIterations) {
    VMTestHarness h;
    // Iterations before the matching one complete the try normally (their
    // own POP_HANDLER runs); only the returning iteration relies on the
    // fix. Looping past it proves the fix does not disturb the ordinary,
    // already-correct path.
    ASSERT_EQ(h.run(R"(
        fun findFirst(n) {
            for (var i = 0; i < n; i = i + 1) {
                try {
                    if (i == 2) { return i; }
                } catch (e) {}
            }
            return -1;
        }
        var r = findFirst(5);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(as<Number>(*h.getGlobal("r")), 2);
}

TEST_F(HandlerStackLeakTest, DeferBeforeReturnFromOpenTryRunsLifoAndCleansUp) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var trace = "";
        fun mark(msg) { trace = trace + msg; }
        fun withDeferAndTry() {
            defer mark("first;");
            defer mark("second;");
            try {
                return "value";
            } catch (e) {}
        }
        var r = withDeferAndTry();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("r"), "value");
    EXPECT_EQ(h.getGlobalStr("trace"), "second;first;");
}

TEST_F(HandlerStackLeakTest, LaterThrowAtSameFrameDepthUsesItsOwnHandler) {
    // The shape from the reported bug: a return from an open try in one
    // call, followed by an unrelated throw/catch in a later call at the
    // same call-frame depth. Before the fix, the second throw could match
    // the first call's leaked record instead of its own.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g(shouldReturn) {
            try {
                if (shouldReturn) { return; }
            } catch (e) { log = log + "g-catch;"; }
        }
        fun hh() {
            try {
                throw "h-fault";
            } catch (e) {
                log = log + "h-catch:" + e + ";";
            }
        }
        g(true);
        hh();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "h-catch:h-fault;");
}

TEST_F(HandlerStackLeakTest, DeferThrowDuringReturnFromOpenTryEscapesToCaller) {
    // The frame's own catch belongs to a try that a `return` already left.
    // A defer registered before that return, if it throws, must not be
    // caught by that same try — see INVARIANT(handler-stack-frame-scoped)
    // on m_handlerStack's declaration.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun boom() { throw "from-defer"; }
        fun g() {
            defer boom();
            try {
                return "early";
            } catch (e) { log = log + "g-wrongly-caught:" + e + ";"; }
        }
        try {
            var r = g();
            log = log + "g-returned:" + r + ";";
        } catch (e) {
            log = log + "outer-caught:" + e + ";";
        }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "outer-caught:from-defer;");
}

TEST_F(HandlerStackLeakTest,
       DeferThrowMidListDuringReturnFromOpenTryStillDrainsEarlierDefers) {
    // Same shape as DeferThrowDuringReturnFromOpenTryEscapesToCaller, but
    // with a defer registered before the one that throws. LIFO order runs
    // the throwing defer first; the earlier-registered one must still run
    // during the resulting unwind (defer's own "always runs" rule), and the
    // throw must still reach the caller's handler, not the frame's own.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var trace = "";
        fun mark(msg) { trace = trace + msg; }
        fun boom() { throw "from-defer"; }
        fun g() {
            defer mark("first;");
            defer boom();
            try {
                return "early";
            } catch (e) { trace = trace + "g-wrongly-caught:" + e + ";"; }
        }
        var caught = "";
        try {
            g();
        } catch (e) {
            caught = e;
        }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("caught"), "from-defer");
    EXPECT_EQ(h.getGlobalStr("trace"), "first;");
}

TEST_F(HandlerStackLeakTest, LaterUncaughtThrowAtSameFrameDepthStaysUncaught) {
    // Same shape as the previous test, but the later throw has no handler
    // at all. Before the fix, this reached a stale HandlerRecord from `g`'s
    // leaked try and crashed (see the issue's own repro); the correct
    // result is an ordinary uncaught-throw RUNTIME_ERROR, and no statement
    // after the throw executes.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun g(shouldReturn) {
            try {
                if (shouldReturn) { return "early"; }
            } catch (e) {}
        }
        fun hh() { throw "h-fault"; }
        var early = g(true);
        hh();
        var after = "reached";
    )"),
              InterpretResult::RUNTIME_ERROR);
    ASSERT_TRUE(h.getGlobal("early").has_value());
    EXPECT_EQ(h.getGlobalStr("early"), "early");
    EXPECT_FALSE(h.getGlobal("after").has_value());
}

// Tests for break/continue handler cleanup (issue #273):

TEST_F(HandlerStackLeakTest, BreakFromOpenTryBodyLeavesNoRecord) {
    // break from inside a try body inside a for loop must pop the try's
    // handler record. The loop's own frame keeps running code after the
    // break, so the record must be popped scoped to the try region, not to
    // the frame. Proves it by throwing before the loop's frame returns,
    // where a stale record would be matched.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g() {
            for (var i = 0; i < 2; i = i + 1) {
                try {
                    if (i == 0) { log = log + "A"; }
                    if (i == 1) { break; }
                } catch (e) { log = log + "B"; }
            }
            throw "after-loop";
        }
        try {
            g();
        } catch (e) { log = log + "C"; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "AC");
}

TEST_F(HandlerStackLeakTest, ContinueFromOpenTryBodyLeavesNoRecord) {
    // continue from inside a try body, at least across 2 loop iterations,
    // must pop the try's handler record each time without leaving stale
    // records. Proves it by throwing before the loop's frame returns.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g() {
            for (var i = 0; i < 3; i = i + 1) {
                try {
                    if (i == 0) { log = log + "A"; }
                    if (i == 1) { continue; }
                    if (i == 2) { continue; }
                } catch (e) { log = log + "B"; }
            }
            throw "after-loop";
        }
        try {
            g();
        } catch (e) { log = log + "C"; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "AC");
}

// Tests for break/continue from catch blocks (issues #286 and #287):
// THROW already removes the record before catch runs, so no POP_HANDLER
// is due on these paths. Each test throws before the frame returns, where
// a wrong cleanup would be matched.

TEST_F(HandlerStackLeakTest, ContinueFromCatchLeavesNoRecord) {
    // Exact shape from issue #286 with continue from catch.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g() {
            var i = 0;
            while (i < 2) {
                try { throw "boom"; } catch (a) { log = log + "first;"; i = i + 1; continue; }
                try { log = log + "second;"; } catch (b) { log = log + "never;"; }
                i = i + 1;
            }
            throw "after-loop";
        }
        try {
            g();
        } catch (e) { log = log + "outer-caught:" + e + ";"; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "first;first;outer-caught:after-loop;");
}

TEST_F(HandlerStackLeakTest, BreakFromCatchLeavesNoRecord) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g() {
            for (var i = 0; i < 3; i = i + 1) {
                try { throw "boom"; } catch (e) { log = log + "catch;"; break; }
            }
            throw "after-loop";
        }
        try {
            g();
        } catch (e) { log = log + "outer-caught:" + e + ";"; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "catch;outer-caught:after-loop;");
}

TEST_F(HandlerStackLeakTest, BreakFromCatchKeepsOuterTryRecord) {
    // Inner catch breaks. Outer try wraps the loop and must still catch.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g() {
            try {
                for (var i = 0; i < 2; i = i + 1) {
                    try { throw "boom"; } catch (e1) { log = log + "inner;"; break; }
                }
                throw "outer-test";
            } catch (e2) { log = log + "outer:" + e2 + ";"; }
        }
        g();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "inner;outer:outer-test;");
}

TEST_F(HandlerStackLeakTest, ContinueFromCatchInForInLoop) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g() {
            for (var item in [1, 2]) {
                try { throw "boom"; } catch (e) { log = log + "caught;"; continue; }
                log = log + "unreached;";
            }
            throw "after-loop";
        }
        try {
            g();
        } catch (e) { log = log + "outer-caught:" + e + ";"; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "caught;caught;outer-caught:after-loop;");
}

TEST_F(HandlerStackLeakTest, ThrowFromCatchEscapesOwnTry) {
    // A throw in catch must reach the outer handler, never the own one.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g() {
            try {
                try { throw "first"; } catch (e) { throw "second"; }
            } catch (outer) { log = log + "outer:" + outer + ";"; }
        }
        g();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "outer:second;");
}

TEST_F(HandlerStackLeakTest, BreakOutOfNestedTryInsideLoop) {
    // break out of a try that is itself nested inside another try, both
    // inside the loop body. The inner try's handler must be popped; the
    // outer try's handler must survive if it's legitimately still open
    // beyond the loop. Proves it by throwing before the frame returns.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g() {
            for (var i = 0; i < 2; i = i + 1) {
                try {
                    try {
                        if (i == 0) { break; }
                    } catch (e1) { log = log + "A"; }
                } catch (e2) { log = log + "B"; }
            }
            throw "after-loop";
        }
        try {
            g();
        } catch (e) { log = log + "C"; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "C");
}

TEST_F(HandlerStackLeakTest, BreakFromInnerTryWhileOuterTryWrapsEntireLoop) {
    // Critical test: a try wrapping the *entire* loop, plus an inner try
    // inside the loop body that breaks. The outer handler record must
    // survive the break and only be popped when the loop exits normally.
    // This is the case a naive "pop by frameCount" fix would get wrong.
    // Proves it by throwing inside the outer try after the loop.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g() {
            try {
                for (var i = 0; i < 2; i = i + 1) {
                    try {
                        if (i == 0) { break; }
                    } catch (e1) { log = log + "A"; }
                }
                throw "outer-test";
            } catch (e2) { log = log + "B"; }
        }
        g();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "B");
}

TEST_F(HandlerStackLeakTest, LaterThrowAfterBreakFromLoop) {
    // The exact shape from issue #273: break from inside a try inside a
    // loop, followed by an unrelated throw after the loop. Before the fix,
    // the second throw was wrongly caught by the loop's stale try handler.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun g() {
            for (var i = 0; i < 3; i = i + 1) {
                try {
                    if (i == 1) { break; }
                } catch (e) { log = log + "loop-catch:" + e + ";"; }
            }
            throw "after-loop";
        }
        try {
            g();
        } catch (e) { log = log + "outer-caught:" + e + ";"; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.handlerStackDepth(), 0);
    EXPECT_EQ(h.getGlobalStr("log"), "outer-caught:after-loop;");
}