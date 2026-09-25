// test_handler_stack_leak.cpp — proves Op::RETURN, break, continue, and
// throw each leave no stale HandlerRecord on m_handlerStack when they exit a
// frame that still has an open try/catch region (see
// INVARIANT(handler-stack-frame-scoped) on m_handlerStack's declaration in
// vm.h).
//
// Invariants under test:
//   1. handlerStackDepth() is 0 after any program whose every try/catch has
//      exited, whether by normal completion, by a caught throw, or by a
//      `return`/`break`/`continue` taken from inside the still-open region
//      (body or catch), at any nesting depth, inside a loop, or alongside a
//      `defer`.
//   2. An early exit from inside an open try does not stop a later,
//      unrelated throw at the same call-frame depth (or the same loop, or a
//      sibling try) from being handled correctly — by its own handler, not
//      by a stale one left behind.
//   3. `defer` registered before an early exit taken from inside an open try
//      still runs, in LIFO order, and does not itself leave a stale handler
//      record.
//   4. A defer that itself throws, when run because of an early exit out of
//      an open try, is not caught by that same try — the record it would
//      otherwise wrongly match is already gone by the time the defer runs —
//      and any earlier-registered defer still runs during the resulting
//      unwind.
//
// One shape (one Lox++ program) proves one specific combination of exit
// path, nesting, and loop/defer interaction. All of them share the same
// checking logic — run the program, check the result, check
// handlerStackDepth(), check the resulting global state — so they are rows
// of ExitPathCase, not separate TEST_F bodies: the shared assertions live
// once, in ExitPathLeavesNoStaleHandlerRecord below, instead of being
// hand-repeated in every row.

#include "test_harness.h"
#include <gtest/gtest.h>

#include <ostream>
#include <string>
#include <vector>

namespace {

// One check against a global left behind by a case's program. StrEq and
// NumEq imply the global is present; Present/Absent check only existence,
// for the one case (an uncaught throw) where reading a value after it isn't
// the point.
struct GlobalCheck {
    enum class Kind { StrEq, NumEq, Present, Absent };

    std::string name;
    Kind kind;
    std::string strValue;
    double numValue = 0;

    static GlobalCheck strEq(std::string n, std::string v) {
        return {std::move(n), Kind::StrEq, std::move(v), 0};
    }
    static GlobalCheck numEq(std::string n, double v) {
        return {std::move(n), Kind::NumEq, "", v};
    }
    static GlobalCheck present(std::string n) {
        return {std::move(n), Kind::Present, "", 0};
    }
    static GlobalCheck absent(std::string n) {
        return {std::move(n), Kind::Absent, "", 0};
    }
};

struct ExitPathCase {
    std::string name;
    std::string source;
    InterpretResult expected;
    // Skipped only for the one case whose program ends in an uncaught
    // runtime error, where the handler stack's own state isn't the point.
    bool checkHandlerDepthZero;
    std::vector<GlobalCheck> checks;
};

// Found by ADL from EXPECT_EQ(..., GetParam()) on failure; without it gtest
// prints ExitPathCase as a raw byte dump instead of the case name.
void PrintTo(const ExitPathCase& c, std::ostream* os) { *os << c.name; }

const std::vector<ExitPathCase>& exitPathCases() {
    static const std::vector<ExitPathCase> cases = {
        {"ReturnFromOpenTryBodyLeavesNoRecord",
         R"(
            fun g(shouldReturn) {
                try {
                    if (shouldReturn) { return "early"; }
                } catch (e) {}
            }
            var r = g(true);
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("r", "early")}},

        // The inner try/catch already pops its own record when it catches
        // "boom" (handleThrow's own bookkeeping). The outer try's record is
        // the one at risk: its POP_HANDLER is only reached on normal
        // fall-through out of the outer block, which the `return` inside
        // the inner catch skips entirely.
        {"ReturnFromCatchLeavesOuterTryRecordPopped",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("r", "returned-from-catch")}},

        {"ReturnFromInnermostOfThreeNestedTriesClearsAll",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("r", "deep")}},

        // Iterations before the matching one complete the try normally
        // (their own POP_HANDLER runs); only the returning iteration relies
        // on the fix. Looping past it proves the fix does not disturb the
        // ordinary, already-correct path.
        {"ReturnFromTryInsideLoopAcrossIterations",
         R"(
            fun findFirst(n) {
                for (var i = 0; i < n; i = i + 1) {
                    try {
                        if (i == 2) { return i; }
                    } catch (e) {}
                }
                return -1;
            }
            var r = findFirst(5);
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::numEq("r", 2)}},

        {"DeferBeforeReturnFromOpenTryRunsLifoAndCleansUp",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("r", "value"),
          GlobalCheck::strEq("trace", "second;first;")}},

        // The shape from the reported bug: a return from an open try in one
        // call, followed by an unrelated throw/catch in a later call at the
        // same call-frame depth. Before the fix, the second throw could
        // match the first call's leaked record instead of its own.
        {"LaterThrowAtSameFrameDepthUsesItsOwnHandler",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "h-catch:h-fault;")}},

        // The frame's own catch belongs to a try that a `return` already
        // left. A defer registered before that return, if it throws, must
        // not be caught by that same try — see
        // INVARIANT(handler-stack-frame-scoped) on m_handlerStack's
        // declaration.
        {"DeferThrowDuringReturnFromOpenTryEscapesToCaller",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "outer-caught:from-defer;")}},

        // Same shape as DeferThrowDuringReturnFromOpenTryEscapesToCaller,
        // but with a defer registered before the one that throws. LIFO
        // order runs the throwing defer first; the earlier-registered one
        // must still run during the resulting unwind (defer's own "always
        // runs" rule), and the throw must still reach the caller's handler,
        // not the frame's own.
        {"DeferThrowMidListDuringReturnFromOpenTryStillDrainsEarlierDefers",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("caught", "from-defer"),
          GlobalCheck::strEq("trace", "first;")}},

        // Same shape as the previous case, but the later throw has no
        // handler at all. Before the fix, this reached a stale
        // HandlerRecord from `g`'s leaked try and crashed (see the issue's
        // own repro); the correct result is an ordinary uncaught-throw
        // RUNTIME_ERROR, and no statement after the throw executes.
        // handlerStackDepth() isn't checked here: the program never
        // completes, so its own state past the crash point isn't the point.
        {"LaterUncaughtThrowAtSameFrameDepthStaysUncaught",
         R"(
            fun g(shouldReturn) {
                try {
                    if (shouldReturn) { return "early"; }
                } catch (e) {}
            }
            fun hh() { throw "h-fault"; }
            var early = g(true);
            hh();
            var after = "reached";
        )",
         InterpretResult::RUNTIME_ERROR,
         false,
         {GlobalCheck::present("early"), GlobalCheck::strEq("early", "early"),
          GlobalCheck::absent("after")}},

        // Tests for break/continue handler cleanup (issue #273):

        // break from inside a try body inside a for loop must pop the try's
        // handler record. The loop's own frame keeps running code after the
        // break, so the record must be popped scoped to the try region, not
        // to the frame. Proves it by throwing before the loop's frame
        // returns, where a stale record would be matched.
        {"BreakFromOpenTryBodyLeavesNoRecord",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "AC")}},

        // continue from inside a try body, at least across 2 loop
        // iterations, must pop the try's handler record each time without
        // leaving stale records. Proves it by throwing before the loop's
        // frame returns.
        {"ContinueFromOpenTryBodyLeavesNoRecord",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "AC")}},

        // Tests for break/continue from catch blocks (issues #286 and
        // #287): THROW already removes the record before catch runs, so no
        // POP_HANDLER is due on these paths. Each case throws before the
        // frame returns, where a wrong cleanup would be matched.

        // Exact shape from issue #286 with continue from catch.
        {"ContinueFromCatchLeavesNoRecord",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "first;first;outer-caught:after-loop;")}},

        {"BreakFromCatchLeavesNoRecord",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "catch;outer-caught:after-loop;")}},

        // Inner catch breaks. Outer try wraps the loop and must still
        // catch.
        {"BreakFromCatchKeepsOuterTryRecord",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "inner;outer:outer-test;")}},

        {"ContinueFromCatchInForInLoop",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "caught;caught;outer-caught:after-loop;")}},

        // A throw in catch must reach the outer handler, never the own one.
        {"ThrowFromCatchEscapesOwnTry",
         R"(
            var log = "";
            fun g() {
                try {
                    try { throw "first"; } catch (e) { throw "second"; }
                } catch (outer) { log = log + "outer:" + outer + ";"; }
            }
            g();
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "outer:second;")}},

        // break out of a try that is itself nested inside another try, both
        // inside the loop body. The inner try's handler must be popped; the
        // outer try's handler must survive if it's legitimately still open
        // beyond the loop. Proves it by throwing before the frame returns.
        {"BreakOutOfNestedTryInsideLoop",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "C")}},

        // Critical shape: a try wrapping the *entire* loop, plus an inner
        // try inside the loop body that breaks. The outer handler record
        // must survive the break and only be popped when the loop exits
        // normally. This is the case a naive "pop by frameCount" fix would
        // get wrong. Proves it by throwing inside the outer try after the
        // loop.
        {"BreakFromInnerTryWhileOuterTryWrapsEntireLoop",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "B")}},

        // The exact shape from issue #273: break from inside a try inside a
        // loop, followed by an unrelated throw after the loop. Before the
        // fix, the second throw was wrongly caught by the loop's stale try
        // handler.
        {"LaterThrowAfterBreakFromLoop",
         R"(
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
        )",
         InterpretResult::OK,
         true,
         {GlobalCheck::strEq("log", "outer-caught:after-loop;")}},
    };
    return cases;
}

} // namespace

class HandlerStackLeakTest : public ::testing::TestWithParam<ExitPathCase> {};

TEST_P(HandlerStackLeakTest, ExitPathLeavesNoStaleHandlerRecord) {
    const ExitPathCase& c = GetParam();
    VMTestHarness h;
    ASSERT_EQ(h.run(c.source), c.expected);
    if (c.checkHandlerDepthZero) {
        EXPECT_EQ(h.handlerStackDepth(), 0);
    }
    for (const GlobalCheck& check : c.checks) {
        switch (check.kind) {
        case GlobalCheck::Kind::StrEq:
            EXPECT_EQ(h.getGlobalStr(check.name), check.strValue)
                << "global " << check.name;
            break;
        case GlobalCheck::Kind::NumEq:
            ASSERT_TRUE(h.getGlobal(check.name).has_value())
                << "global " << check.name;
            EXPECT_EQ(as<Number>(*h.getGlobal(check.name)), check.numValue)
                << "global " << check.name;
            break;
        case GlobalCheck::Kind::Present:
            EXPECT_TRUE(h.getGlobal(check.name).has_value())
                << "global " << check.name;
            break;
        case GlobalCheck::Kind::Absent:
            EXPECT_FALSE(h.getGlobal(check.name).has_value())
                << "global " << check.name;
            break;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    ExitPaths, HandlerStackLeakTest, ::testing::ValuesIn(exitPathCases()),
    [](const ::testing::TestParamInfo<ExitPathCase>& info) {
        return info.param.name;
    });
