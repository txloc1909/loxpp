// test_coroutine.cpp — coroutine primitive invariant tests.
//
// Covers the native VM's copy-on-suspend coroutine state (mission #523 node
// #526): yield/resume across a call boundary, resume-with-value, the three
// catchable protocol faults, the four states (including `normal`), defer
// behaviour across a suspension, and an open upvalue shared with an escaping
// closure. The `_gc` twin runs the whole suite under LOXPP_STRESS_GC so the
// snapshot's GC rooting is exercised too.

#include "test_harness.h"

#include <gtest/gtest.h>
#include <string>

static void expect_num(const Value& v, double expected) {
    ASSERT_TRUE(is<Number>(v)) << "expected Number";
    EXPECT_NEAR(as<Number>(v), expected, 1e-9);
}

static void expect_string(const VMTestHarness& h, const std::string& name,
                          const std::string& expected) {
    auto v = h.getGlobal(name);
    ASSERT_TRUE(v.has_value()) << "global '" << name << "' is undefined";
    ASSERT_TRUE(isString(*v)) << "global '" << name << "' is not a String";
    const auto& chars = asObjString(as<Obj*>(*v))->chars;
    EXPECT_EQ(std::string(chars.data(), chars.size()), expected);
}

class CoroutineTest : public ::testing::Test {};

TEST_F(CoroutineTest, YieldAcrossCallBoundary) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun inner() {
            yield "from-inner";
            return "inner-done";
        }
        fun outer() {
            var v = inner();
            return v;
        }
        var co = coroutine.create(outer);
        var s0 = co.status();
        var r1 = co.resume();
        var s1 = co.status();
        var r2 = co.resume();
        var s2 = co.status();
    )"),
              InterpretResult::OK);
    expect_string(h, "s0", "suspended");
    expect_string(h, "r1", "from-inner");
    expect_string(h, "s1", "suspended");
    expect_string(h, "r2", "inner-done");
    expect_string(h, "s2", "dead");
}

TEST_F(CoroutineTest, ResumeSendsValueIntoPendingYield) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun gen() {
            var a = yield 1;
            var b = yield 2;
            return a + b;
        }
        var co = coroutine.create(gen);
        var r1 = co.resume();
        var r2 = co.resume(10);
        var r3 = co.resume(32);
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r1"), 1);
    expect_num(*h.getGlobal("r2"), 2);
    expect_num(*h.getGlobal("r3"), 42);
}

TEST_F(CoroutineTest, FirstResumePassesArgumentsAndChecksArity) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun add(a, b) {
            yield a + b;
        }
        var co = coroutine.create(add);
        var sum = co.resume(2, 3);

        var kind = "";
        var bad = coroutine.create(add);
        try {
            bad.resume(1);
        } catch (e) {
            kind = e.kind;
        }
        var still = bad.status();
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("sum"), 5);
    expect_string(h, "kind", "ArityError");
    expect_string(h, "still", "suspended");
}

TEST_F(CoroutineTest, ResumeDeadIsCatchable) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f() {
            return 1;
        }
        var co = coroutine.create(f);
        co.resume();
        var kind = "";
        try {
            co.resume();
        } catch (e) {
            kind = e.kind;
        }
    )"),
              InterpretResult::OK);
    expect_string(h, "kind", "DeadCoroutineError");
}

TEST_F(CoroutineTest, ResumeRunningIsCatchable) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var self;
        fun f() {
            self.resume();
        }
        self = coroutine.create(f);
        var kind = "";
        try {
            self.resume();
        } catch (e) {
            kind = e.kind;
        }
    )"),
              InterpretResult::OK);
    expect_string(h, "kind", "RunningCoroutineError");
}

TEST_F(CoroutineTest, YieldOutsideCoroutineIsCatchable) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var kind = "";
        try {
            yield 1;
        } catch (e) {
            kind = e.kind;
        }
    )"),
              InterpretResult::OK);
    expect_string(h, "kind", "YieldOutsideCoroutineError");
}

TEST_F(CoroutineTest, NestedResumeSeesNormalState) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var outer;
        fun innerFn() {
            var s = outer.status();
            yield s;
        }
        fun outerFn() {
            var ic = coroutine.create(innerFn);
            return ic.resume();
        }
        outer = coroutine.create(outerFn);
        var seen = outer.resume();
    )"),
              InterpretResult::OK);
    expect_string(h, "seen", "normal");
}

TEST_F(CoroutineTest, YieldDoesNotRunDefers) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun announce() {
            log = log + "cleanup";
        }
        fun worker() {
            defer announce();
            yield "tick";
            log = log + "resumed";
            return "done";
        }
        var co = coroutine.create(worker);
        var r1 = co.resume();
        var mid = log;
        var r2 = co.resume();
    )"),
              InterpretResult::OK);
    expect_string(h, "r1", "tick");
    expect_string(h, "mid", "");
    expect_string(h, "r2", "done");
    expect_string(h, "log", "resumedcleanup");
}

TEST_F(CoroutineTest, AbandonedCoroutineRunsNoDefer) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun announce() {
            log = log + "x";
        }
        fun worker() {
            defer announce();
            yield "suspend";
        }
        var co = coroutine.create(worker);
        co.resume();
    )"),
              InterpretResult::OK);
    expect_string(h, "log", "");
}

// A closure that escapes a suspending coroutine shares the coroutine's live
// local through an open upvalue. The upvalue must stay bound to the same cell
// while the coroutine is suspended, and the coroutine must observe writes the
// escaping closure made through it.
TEST_F(CoroutineTest, EscapingClosureSharesOpenUpvalueAcrossSuspend) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun make() {
            var n = 0;
            fun bump() {
                n = n + 1;
                return n;
            }
            yield bump;
            return n;
        }
        var co = coroutine.create(make);
        var f = co.resume();
        var a = f();
        var b = f();
        var c = co.resume();
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 1);
    expect_num(*h.getGlobal("b"), 2);
    expect_num(*h.getGlobal("c"), 2);
}

// An escaping closure must keep a suspended coroutine's snapshot alive even
// after every direct reference to the coroutine is gone: the upvalue's cell
// lives in that snapshot, so the coroutine must be traced through the
// upvalue. Under LOXPP_STRESS_GC this is a use-after-free if it is not.
TEST_F(CoroutineTest, EscapingClosureKeepsSuspendedCoroutineAlive) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun make() {
            var n = 0;
            fun bump() {
                n = n + 1;
                var tag = "n" + str(n);
                return n;
            }
            yield bump;
            return n;
        }
        fun get() {
            var co = coroutine.create(make);
            return co.resume();
        }
        var f = get();
        var a = f();
        var b = f();
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 1);
    expect_num(*h.getGlobal("b"), 2);
}

// A suspension must not reorder the coroutine's live try/catch records. The
// innermost handler has to win after a resume, exactly as it does without a
// yield in between.
TEST_F(CoroutineTest, NestedHandlersKeepTheirOrderAcrossYield) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f() {
            try {
                try {
                    yield 1;
                    throw "boom";
                } catch (e) {
                    return "inner " + e;
                }
            } catch (e) {
                return "outer " + e;
            }
        }
        var co = coroutine.create(f);
        var r1 = co.resume();
        var r2 = co.resume();
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r1"), 1);
    expect_string(h, "r2", "inner boom");
}

// A first resume whose function returns without yielding must leave the
// resumer's operand stack exactly where it found it. Otherwise later locals,
// loops, and for-in read the wrong slots.
TEST_F(CoroutineTest, FirstResumeThatCompletesLeavesStackIntact) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f() {
            return 1;
        }
        var co = coroutine.create(f);
        var r = co.resume();
        var sum = 0;
        for (var i = 0; i < 3; i = i + 1) {
            sum = sum + i;
        }
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 1);
    expect_num(*h.getGlobal("sum"), 3);
}

// The same invariant for a first resume that passes arguments: the resume
// window is wider, so a wrong stack top would shift by more than one slot.
TEST_F(CoroutineTest, FirstResumeWithArgsThatCompletesLeavesStackIntact) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun add(a, b) {
            return a + b;
        }
        var co = coroutine.create(add);
        var r = co.resume(2, 3);
        var sum = 0;
        for (var x in [10, 20, 30]) {
            sum = sum + x;
        }
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 5);
    expect_num(*h.getGlobal("sum"), 60);
}

// A handler in the resume caller's own frame must stay live across the
// coroutine's yield. Suspend must not capture it into the snapshot.
TEST_F(CoroutineTest, ResumerHandlerSurvivesYield) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f() {
            yield 1;
        }
        var co = coroutine.create(f);
        var got = "";
        try {
            co.resume();
            throw "after";
        } catch (e) {
            got = e;
        }
    )"),
              InterpretResult::OK);
    expect_string(h, "got", "after");
}

// The wrong-handler shape: the coroutine's own later throw must reach the
// innermost handler of the resumer, not an outer one.
TEST_F(CoroutineTest, ResumerInnermostHandlerStillWins) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f() {
            yield 1;
            throw "boom";
        }
        var co = coroutine.create(f);
        var got = "";
        try {
            co.resume();
            try {
                co.resume();
            } catch (e) {
                got = "inner " + e;
            }
        } catch (e) {
            got = "outer " + e;
        }
    )"),
              InterpretResult::OK);
    expect_string(h, "got", "inner boom");
}

// The resumer is itself a coroutine: its handler must survive the inner
// coroutine's yield, and its own throw must be caught by it.
TEST_F(CoroutineTest, NestedResumerHandlerSurvivesInnerYield) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun inner() {
            yield 1;
        }
        fun outer() {
            var ic = coroutine.create(inner);
            try {
                ic.resume();
                throw "after";
            } catch (e) {
                return "outer caught " + e;
            }
            return "no";
        }
        var co = coroutine.create(outer);
        var r = co.resume();
    )"),
              InterpretResult::OK);
    expect_string(h, "r", "outer caught after");
}

TEST_F(CoroutineTest, TypeAndStringify) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f() {
            return 1;
        }
        var co = coroutine.create(f);
        var t = type(co);
        var s = str(co);
        var same = co == co;
    )"),
              InterpretResult::OK);
    expect_string(h, "t", "Coroutine");
    expect_string(h, "s", "<coroutine>");
    auto same = h.getGlobal("same");
    ASSERT_TRUE(is<bool>(*same));
    EXPECT_TRUE(as<bool>(*same));
}
