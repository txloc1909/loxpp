// test_task.cpp — tasks on one worker (spec/04-semantics.md, Workers and
// Tasks).
//
// The spec probes 66 to 86 in test/translation-probes/ pin the observable
// rules. These tests cover the paths of the native scheduler that the probes
// do not reach: a receive inside a coroutine that a for-in or a second
// coroutine resumed, a throw after the wake, the native-callback rule through
// a defer and through a native, deadlock on one worker, the names in the
// deadlock report of the credit channel probe, and the main task across REPL
// lines. The `_gc` twin runs the suite under LOXPP_STRESS_GC.

#include "corpus.h"
#include "test_harness.h"

#include <gtest/gtest.h>
#include <string>

static void expect_string(const VMTestHarness& h, const std::string& name,
                          const std::string& expected) {
    auto v = h.getGlobal(name);
    ASSERT_TRUE(v.has_value()) << "global '" << name << "' is undefined";
    ASSERT_TRUE(isString(*v)) << "global '" << name << "' is not a String";
    const auto& chars = asObjString(as<Obj*>(*v))->chars;
    EXPECT_EQ(std::string(chars.data(), chars.size()), expected);
}

class TaskTest : public ::testing::Test {};

// ITER_HAS_NEXT resumes the coroutine; a receive in it suspends the task at
// that opcode, and the wake finishes the opcode with the next element.
TEST_F(TaskTest, ReceiveInsideForInSuspendsTheTask) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        var main = task.self();
        fun gen() {
            yield task.receive();
            yield task.receive();
        }
        fun consumer() {
            for (var x in coroutine.create(gen)) {
                log = log + "got " + x + ";";
            }
            task.send(main, "end");
        }
        var c = task.spawn(consumer);
        yield;
        log = log + "main;";
        task.send(c, "a");
        task.send(c, "b");
        var last = task.receive();
        log = log + last;
    )"),
              InterpretResult::OK);
    expect_string(h, "log", "main;got a;got b;end");
}

// Two coroutines between the task's base and the receive: both wait, with
// the statuses "normal" (the outer one) and "running" (the one that holds the
// receive). The receive waits twice, so the chain is rebuilt twice.
TEST_F(TaskTest, ReceiveTwoCoroutinesDeepWaitsTwice) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        var main = task.self();
        var outer;
        var inner;
        fun innerBody() {
            var a = task.receive();
            var b = task.receive();
            return a + b;
        }
        fun outerBody() {
            inner = coroutine.create(innerBody);
            var r = inner.resume();
            yield "outer got " + r;
        }
        fun owner() {
            outer = coroutine.create(outerBody);
            var r = outer.resume();
            log = log + r + ";";
            task.send(main, "done");
        }
        var t = task.spawn(owner);
        yield;
        log = log + outer.status() + "," + inner.status() + ";";
        task.send(t, "x");
        yield;
        log = log + outer.status() + "," + inner.status() + ";";
        task.send(t, "y");
        var last = task.receive();
        log = log + last;
    )"),
              InterpretResult::OK);
    expect_string(h, "log", "normal,running;normal,running;outer got xy;done");
}

// A throw that leaves the inner coroutine after the wake reaches the try
// around the resume in the task, as it would with no task switch.
TEST_F(TaskTest, ThrowAfterTheWakeIsCaughtInTheTask) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        var main = task.self();
        fun body() {
            var m = task.receive();
            throw "bad " + m;
        }
        fun owner() {
            var co = coroutine.create(body);
            try {
                co.resume();
                log = log + "no throw;";
            } catch (e) {
                log = log + "caught " + e + ";" + co.status() + ";";
            }
            task.send(main, "done");
        }
        var t = task.spawn(owner);
        yield;
        task.send(t, "m");
        var last = task.receive();
        log = log + last;
    )"),
              InterpretResult::OK);
    expect_string(h, "log", "caught bad m;dead;done");
}

// A deferred call runs in a re-entrant run, so a receive there that would
// suspend raises, as a yield there does.
TEST_F(TaskTest, ReceiveInADeferredCallRaises) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var kind = "none";
        fun wait() {
            try {
                task.receive();
            } catch (e) {
                kind = e.kind;
            }
        }
        fun f() {
            defer wait();
        }
        f();
    )"),
              InterpretResult::OK);
    expect_string(h, "kind", "YieldAcrossNativeError");
}

// A native between the task and the receive holds a C++ continuation that a
// task switch would lose, so the receive raises.
TEST_F(TaskTest, ReceiveThroughANativeRaises) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var kind = "none";
        var message = "";
        try {
            callMethod(task, "receive");
        } catch (e) {
            kind = e.kind;
            message = e.message;
        }
    )"),
              InterpretResult::OK);
    expect_string(h, "kind", "YieldAcrossNativeError");
    expect_string(h, "message", "Cannot receive across a native callback.");
}

// A `resume` native that another native calls is not a clean link either.
TEST_F(TaskTest, ReceiveUnderAResumeThatANativeCalledRaises) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var kind = "none";
        fun body() {
            try {
                task.receive();
            } catch (e) {
                kind = e.kind;
            }
        }
        class Holder {}
        var holder = Holder();
        holder.run = coroutine.create(body).resume;
        callMethod(holder, "run");
    )"),
              InterpretResult::OK);
    expect_string(h, "kind", "YieldAcrossNativeError");
}

// One worker: when the main task waits and no task can run, nothing can ever
// send. DeadlockError is fatal: the try does not catch it and the deferred
// call does not run.
TEST_F(TaskTest, DeadlockOnOneWorkerIsFatal) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(
        var log = "start";
        fun note() {
            log = log + ";deferred";
        }
        fun waiter() {
            task.receive();
        }
        fun run() {
            defer note();
            task.spawn(waiter);
            try {
                task.receive();
            } catch (e) {
                log = log + ";caught";
            }
        }
        run();
    )"),
              InterpretResult::RUNTIME_ERROR);
    expect_string(h, "log", "start");
}

// The credit channel probe ends in DeadlockError with the producer waiting
// for a credit in its own receive. The spec lets the report list the tasks
// that wait; the native report names both, by the function each task runs.
TEST_F(TaskTest, CreditChannelDeadlockNamesProducerAndConsumer) {
    const std::string source = loxpp_test::readFile(
        loxpp_test::projectRoot() /
        "test/translation-probes/91_credit_channel_deadlock.lox");
    VMTestHarness h;
    testing::internal::CaptureStderr();
    testing::internal::CaptureStdout();
    InterpretResult result = h.run(source);
    std::string out = testing::internal::GetCapturedStdout();
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, InterpretResult::RUNTIME_ERROR);
    EXPECT_NE(err.find("Deadlock: every task waits in receive.\n"),
              std::string::npos)
        << err;
    EXPECT_NE(err.find("  task script on worker 0 waits in receive\n"),
              std::string::npos)
        << err;
    EXPECT_NE(err.find("  task producer on worker 0 waits in receive\n"),
              std::string::npos)
        << err;
    EXPECT_NE(out.find("send 4\n"), std::string::npos) << out;
    EXPECT_EQ(out.find("sent 4\n"), std::string::npos) << out;
}

// A function that no top-level fun declaration made is not an entry: a
// nested fun, a bound method, and a number all fail before a worker starts.
// The fault is fatal, so the try does not catch it.
TEST_F(TaskTest, SpawnWorkerTakesOnlyATopLevelFunction) {
    const char* entries[] = {"nested()", "C().m", "42"};
    for (const char* entry : entries) {
        VMTestHarness h;
        std::string program = R"(
            fun nested() {
                fun inner(m) {}
                return inner;
            }
            class C { m(x) {} }
            var reached = "before";
            try {
                task.spawn_worker()";
        program += entry;
        program += R"(, nil);
            } catch (e) {
                reached = "caught";
            }
            reached = "after";
        )";
        EXPECT_EQ(h.run(program), InterpretResult::RUNTIME_ERROR) << entry;
        expect_string(h, "reached", "before");
    }
}

// The bit is the conjunction of the field bits, computed once at the
// constructor, so a message nested in a message is one, at any depth.
TEST_F(TaskTest, SendableBitFollowsTheFields) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { Box(v) Pair(a, b) None }
        fun f() {}
        var me = task.self();
        fun sendable(v) {
            try {
                task.send(me, v);
                task.receive();
                return "yes";
            } catch (e) {
                return "no";
            }
        }
        var log = sendable(E.Box(E.Box(E.Box(me)))) + sendable(E.Pair(E.None(), "s")) +
            sendable(E.Box(E.Box(E.Box({})))) + sendable(E.Pair(1, E.Box(f)));
    )"),
              InterpretResult::OK);
    expect_string(h, "log", "yesyesnono");
}

// Each REPL line runs as the main task. Its handle stays the same, and a task
// that is left waiting when a line ends has ended with that line.
TEST_F(TaskTest, MainTaskOutlivesALineAndOtherTasksDoNot) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var first = task.self();
        fun waiter() {
            task.receive();
        }
        var w = task.spawn(waiter);
        yield;
    )"),
              InterpretResult::OK);
    ASSERT_EQ(h.run(R"(
        var same = task.self() == first;
        var kind = "none";
        try {
            task.send(w, 1);
        } catch (e) {
            kind = e.kind;
        }
    )"),
              InterpretResult::OK);
    auto same = h.getGlobal("same");
    ASSERT_TRUE(same.has_value());
    EXPECT_TRUE(is<bool>(*same) && as<bool>(*same));
    expect_string(h, "kind", "DeadTaskError");
}

// A task that resumes a coroutine another task made: the coroutine runs in
// the task that resumes it, and its own yields go to that task.
TEST_F(TaskTest, ACoroutineRunsInTheTaskThatResumesIt) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var main = task.self();
        fun gen() {
            yield task.self();
            yield task.self();
        }
        var co = coroutine.create(gen);
        var a = co.resume();
        fun other() {
            task.send(main, co.resume());
        }
        var t = task.spawn(other);
        var b = task.receive();
        var log = str(a == main) + str(b == t);
    )"),
              InterpretResult::OK);
    expect_string(h, "log", "truetrue");
}

// A spawned task whose entry is a native function cannot park in receive.
// Nothing can rebuild the native frame, so the task ends with an uncaught
// YieldAcrossNativeError (spec/04-semantics.md, task.receive step 3).
TEST_F(TaskTest, NativeEntryReceiveOnEmptyMailboxIsUncaught) {
    VMTestHarness h;
    testing::internal::CaptureStderr();
    InterpretResult result = h.run(R"(
        task.spawn(task.receive);
        yield;
    )");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, InterpretResult::RUNTIME_ERROR);
    EXPECT_NE(err.find("Cannot receive across a native callback."),
              std::string::npos)
        << err;
}

// A receive_where call that waits runs again in the frames of the task, also
// when two coroutines lie between the task and the call. Each wake tests the
// new message once, and a message that does not match stays in order.
TEST_F(TaskTest, ReceiveWhereWaitsInsideTwoCoroutines) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        var main = task.self();
        fun isText(m) { return type(m) == "String"; }
        fun innerBody() {
            return task.receive_where(isText);
        }
        fun outerBody() {
            var inner = coroutine.create(innerBody);
            yield "outer got " + inner.resume();
        }
        fun owner() {
            var outer = coroutine.create(outerBody);
            log = log + outer.resume() + ";";
            task.send(main, task.pending());
        }
        var t = task.spawn(owner);
        yield;
        task.send(t, 1);
        yield;
        task.send(t, 2);
        yield;
        task.send(t, "x");
        var left = task.receive();
        log = log + "left " + str(left);
    )"),
              InterpretResult::OK);
    expect_string(h, "log", "outer got x;left 2");
}

// A throw out of `want` on a later pass reaches the try around the call, and
// the message that `want` rejected or threw on stays in the mailbox.
TEST_F(TaskTest, ReceiveWhereThrowAfterTheWakeIsCaughtInTheTask) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        var main = task.self();
        fun pick(m) {
            if (m == 2) {
                throw "bad";
            }
            return false;
        }
        fun owner() {
            try {
                task.receive_where(pick);
            } catch (e) {
                log = log + "caught " + e + ";";
            }
            task.send(main, task.pending());
        }
        var t = task.spawn(owner);
        yield;
        task.send(t, 1);
        yield;
        task.send(t, 2);
        var left = task.receive();
        log = log + "left " + str(left);
    )"),
              InterpretResult::OK);
    expect_string(h, "log", "caught bad;left 2");
}

// A limit that passes with no matching message raises TimeoutError in the
// task, and the unmatched message is still there.
TEST_F(TaskTest, ReceiveWhereWithinRaisesTimeoutErrorAndKeepsMessages) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        fun never(m) { return false; }
        task.send(task.self(), "kept");
        try {
            task.receive_where_within(never, 0.05);
        } catch (e) {
            log = e.kind + ";" + task.receive();
        }
    )"),
              InterpretResult::OK);
    expect_string(h, "log", "TimeoutError;kept");
}

// The `want` function and its closure stay reachable while the task waits.
TEST_F(TaskTest, ReceiveWhereKeepsItsFunctionAliveAcrossACollection) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var log = "";
        var main = task.self();
        // Only the waiting call holds the function that makeWanted returns.
        fun makeWanted(text) {
            fun isWanted(m) { return m == text; }
            return isWanted;
        }
        fun owner() {
            task.send(main, task.receive_where(makeWanted("w" + str(1 + 1))));
        }
        var t = task.spawn(owner);
        yield;
        var junk = [];
        for (var i = 0; i < 2000; i = i + 1) { junk.append("j" + str(i)); }
        task.send(t, "w2");
        log = task.receive();
    )"),
              InterpretResult::OK);
    expect_string(h, "log", "w2");
}
