// test_park.cpp — a task that suspends in a native call that waits for time or
// a descriptor (Runtime::parkCurrentTask).
//
// The spec probes 260, 279, 282, 283 and 272 to 277 pin what a program sees of
// `sleep`. These tests look inside the park: the pending value and the
// pending fault of a parked task, the roots, the mailbox, the end of the
// program, and the idle count of a worker. The tests use a native
// `park_far()` that parks the current task for 30 seconds, and a host timer
// that ends the park from the poll handler, because only the host can inject
// a value or a fault. The `_gc` twin runs the suite under LOXPP_STRESS_GC.

#include "memory_manager.h"
#include "objects.h"
#include "test_harness.h"
#include "vm.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using PClock = Poller::Clock;

ObjTask* g_parked = nullptr;
int g_parkFd = -1;

// park_far(): parks for 30 seconds. Returns -1 where the task cannot suspend,
// after it recorded the task.
Value parkFarNative(int /*argc*/, Value* /*argv*/) {
    Runtime* rt = getActiveRuntime();
    g_parked = rt->currentTask();
    if (!rt->parkCurrentTask(ParkReason::TIMER, -1,
                             PClock::now() + std::chrono::seconds(30))) {
        return from<Number>(-1.0);
    }
    return from<Nil>(Nil{});
}

// park_fd(): parks until g_parkFd is readable.
Value parkFdNative(int /*argc*/, Value* /*argv*/) {
    Runtime* rt = getActiveRuntime();
    g_parked = rt->currentTask();
    if (!rt->parkCurrentTask(ParkReason::FD_READ, g_parkFd)) {
        return from<Number>(-1.0);
    }
    return from<Nil>(Nil{});
}

std::string globalString(const VM& vm, const std::string& name) {
    auto v = vm.getGlobal(name);
    if (!v.has_value()) {
        return "<undefined>";
    }
    return stringify(*v);
}

// A VM whose `park_far` parks, and whose host timer calls `act` 80 ms after
// the program starts, with the task that parked.
class ParkRig {
  public:
    explicit ParkRig(std::function<void(Runtime&, ObjTask*)> act)
        : m_act(std::move(act)) {
        g_parked = nullptr;
        Runtime& rt = m_vm.runtime();
        rt.defineNative("park_far", parkFarNative, 0);
        m_hostWait = rt.poller().waitUntil(PClock::now() +
                                           std::chrono::milliseconds(80));
        rt.setPollHandler([this, &rt](const std::vector<Poller::Token>& ready) {
            for (Poller::Token token : ready) {
                if (token == m_hostWait) {
                    ASSERT_NE(g_parked, nullptr);
                    m_act(rt, g_parked);
                }
            }
        });
    }
    VM& vm() { return m_vm; }

  private:
    VM m_vm;
    std::function<void(Runtime&, ObjTask*)> m_act;
    Poller::Token m_hostWait{0};
};

} // namespace

class ParkTest : public ::testing::Test {};

// A fault that a parked call carries is raised in the task when it runs
// again, and `catch` of the task receives it.
TEST_F(ParkTest, PendingFaultReachesCatchInTheTask) {
    ParkRig rig([](Runtime& rt, ObjTask* task) {
        rt.failPark(task, "SocketClosedError", "the wait failed");
    });
    ASSERT_EQ(rig.vm().interpret(R"(
        var got = "none";
        var me = task.self();
        fun t() {
            try {
                park_far();
                got = "returned";
            } catch (e) {
                got = e.kind + ":" + e.message;
            }
            task.send(me, "done");
        }
        task.spawn(t);
        task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(rig.vm(), "got"),
              "SocketClosedError:the wait failed");
    EXPECT_FALSE(rig.vm().runtime().poller().hasWaits());
}

// The fault leaves the innermost coroutine and is caught in the task's own
// frame, through a generator that holds the parked call. No deferred call
// runs at the park: the defer runs while the fault unwinds, after the park.
TEST_F(ParkTest, PendingFaultCrossesACoroutineAndRunsDefersThenNotBefore) {
    std::string atPark = "<unset>";
    ParkRig rig([&](Runtime& rt, ObjTask* task) {
        auto log = rt.getGlobal("log");
        atPark = log.has_value() ? stringify(*log) : "<undefined>";
        rt.failPark(task, "SocketClosedError", "gone");
    });
    ASSERT_EQ(rig.vm().interpret(R"(
        var log = "";
        var me = task.self();
        fun note(text) { log = log + text; }
        fun gen() {
            defer note("defer;");
            park_far();
            log = log + "after park;";
        }
        fun t() {
            var g = coroutine.create(gen);
            try {
                g.resume();
            } catch (e) {
                log = log + "caught " + e.kind + ";";
            }
            task.send(me, "done");
        }
        task.spawn(t);
        task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(atPark, "");
    EXPECT_EQ(globalString(rig.vm(), "log"), "defer;caught SocketClosedError;");
}

// A fault with no handler ends the program with the message, and not with an
// internal error.
TEST_F(ParkTest, PendingFaultWithNoHandlerEndsTheProgram) {
    ParkRig rig([](Runtime& rt, ObjTask* task) {
        rt.failPark(task, "SocketClosedError", "nobody catches this");
    });
    testing::internal::CaptureStderr();
    InterpretResult result = rig.vm().interpret(R"(
        var me = task.self();
        fun t() { park_far(); }
        task.spawn(t);
        task.receive();
    )");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, InterpretResult::RUNTIME_ERROR);
    EXPECT_NE(err.find("nobody catches this"), std::string::npos) << err;
    EXPECT_EQ(err.find("BUG"), std::string::npos) << err;
}

// A handler in the coroutine that resumed the generator catches a fault that
// the generator's parked call raised: the fault crosses a coroutine boundary
// and the resume of the chain continues.
TEST_F(ParkTest, PendingFaultIsCaughtByAnOuterCoroutine) {
    ParkRig rig([](Runtime& rt, ObjTask* task) {
        rt.failPark(task, "SocketClosedError", "outer");
    });
    ASSERT_EQ(rig.vm().interpret(R"(
        var got = "none";
        var me = task.self();
        fun inner() { park_far(); yield 1; }
        fun outer() {
            var g = coroutine.create(inner);
            try { g.resume(); } catch (e) { got = "outer caught " + e.message; }
            yield 2;
        }
        fun t() {
            var c = coroutine.create(outer);
            c.resume();
            task.send(me, "done");
        }
        task.spawn(t);
        task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(rig.vm(), "got"), "outer caught outer");
}

// The pending value is the result of the parked call, through a generator
// that holds the call, and under collection: the value is a new string that
// only the task refers to.
TEST_F(ParkTest, PendingValueIsTheResultOfTheCall) {
    ParkRig rig([](Runtime& rt, ObjTask* task) {
        // Only the parked set refers to the task now.
        rt.memoryManager().collectGarbage();
        Value text{static_cast<Obj*>(
            rt.memoryManager().makeString("value from the host"))};
        rt.completePark(task, text);
        // Only the task refers to the string now.
        rt.memoryManager().collectGarbage();
    });
    ASSERT_EQ(rig.vm().interpret(R"(
        var got = "none";
        var me = task.self();
        fun gen() { yield park_far() + "!"; }
        fun t() {
            for (var x in coroutine.create(gen)) { got = x; }
            task.send(me, "done");
        }
        task.spawn(t);
        task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(rig.vm(), "got"), "value from the host!");
    // The end of the park cancelled the 30 second wait.
    EXPECT_FALSE(rig.vm().runtime().poller().hasWaits());
}

// task.pending() reads the mailbox only: the pending result is not a
// message. A message that arrives while the task is parked does not wake it,
// and it is still in the mailbox when the park ends.
TEST_F(ParkTest, ASendDoesNotWakeAParkedTask) {
    ParkRig rig([](Runtime& rt, ObjTask* task) {
        // The send ran during the park. It left the task parked, with the
        // message in its mailbox.
        EXPECT_EQ(task->state, TaskState::PARKED);
        EXPECT_EQ(task->mailbox.size(), 1u);
        EXPECT_NE(task->parkReason, ParkReason::NONE);
        rt.completePark(task, from<Number>(42.0));
    });
    ASSERT_EQ(rig.vm().interpret(R"(
        var me = task.self();
        var sleeper;
        var during = -1;
        var value = -1;
        var after = -1;
        var message = "none";
        fun s() {
            during = task.pending();
            value = park_far();
            after = task.pending();
            message = task.receive();
            task.send(me, "done");
        }
        fun sender() { task.send(sleeper, "m"); }
        sleeper = task.spawn(s);
        task.spawn(sender);
        task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(rig.vm(), "during"), "0");
    EXPECT_EQ(globalString(rig.vm(), "value"), "42");
    EXPECT_EQ(globalString(rig.vm(), "after"), "1");
    EXPECT_EQ(globalString(rig.vm(), "message"), "m");
}

// Where the task cannot suspend, park_far() returns at once with no park.
TEST_F(ParkTest, ParkWhereTheTaskCannotSuspendReturnsFalse) {
    ParkRig rig([](Runtime&, ObjTask*) { FAIL() << "nothing is parked"; });
    ASSERT_EQ(rig.vm().interpret(R"(
        var got = "none";
        class Slow { __str__() { got = park_far(); return "s"; } }
        var text = str(Slow());
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(rig.vm(), "got"), "-1");
}

// When the main task ends, a task that is still parked is abandoned and its
// wait is cancelled: the worker holds no wait afterwards.
TEST_F(ParkTest, TheEndOfTheProgramAbandonsParkedTasksAndTheirWaits) {
    VM vm;
    const auto start = PClock::now();
    ASSERT_EQ(vm.interpret(R"(
        var started = false;
        fun t() { started = true; sleep(60); }
        task.spawn(t);
        yield;
    )"),
              InterpretResult::OK);
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::seconds>(PClock::now() - start)
            .count(),
        20);
    EXPECT_EQ(globalString(vm, "started"), "true");
    ASSERT_TRUE(vm.runtime().hasPoller());
    EXPECT_FALSE(vm.runtime().poller().hasWaits());
}

// A task that is ready does not starve a wait that is over: two tasks pass a
// message to each other for as long as the program lets them, and the
// sleeper still wakes while they do it. `seen` is read when the passing
// ends, before the queue runs empty and the sleeper could wake by that.
TEST_F(ParkTest, ARunnableTaskDoesNotStarveASleeper) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        var me = task.self();
        var woke = false;
        var seen = false;
        var begin = time();
        fun sleeper() { sleep(0.2); woke = true; }
        fun player(other) {
            while (true) {
                var m = task.receive();
                if (m == "stop") { task.send(me, "done"); return; }
                if (woke or time() - begin >= 3) {
                    seen = woke;
                    task.send(other, "stop");
                    task.send(me, "done");
                    return;
                }
                task.send(other, m);
            }
        }
        var b;
        var c;
        fun first() { player(c); }
        fun second() { player(b); }
        task.spawn(sleeper);
        b = task.spawn(first);
        c = task.spawn(second);
        task.send(b, "ball");
        task.receive();
        task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "seen"), "true");
}

// The wait on a descriptor: the task leaves the run queue until the
// descriptor is readable, and the call continues then.
TEST_F(ParkTest, ADescriptorWaitEndsWhenTheDescriptorIsReadable) {
    int fds[2];
    ASSERT_EQ(::pipe2(fds, O_CLOEXEC | O_NONBLOCK), 0);
    g_parkFd = fds[0];
    VM vm;
    vm.runtime().defineNative("park_fd", parkFdNative, 0);
    std::thread writer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        ssize_t n = ::write(fds[1], "x", 1);
        (void)n;
    });
    const auto start = PClock::now();
    InterpretResult result = vm.interpret(R"(
        var me = task.self();
        var log = "";
        fun t() { log = log + "park;"; park_fd(); log = log + "woke;"; task.send(me, 1); }
        fun other() { log = log + "other;"; }
        task.spawn(t);
        task.spawn(other);
        task.receive();
    )");
    writer.join();
    ::close(fds[0]);
    ::close(fds[1]);
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "park;other;woke;");
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(
                  PClock::now() - start)
                  .count(),
              140);
}

// epoll refuses a regular file. Such a descriptor is always ready, so the
// call continues at once and does not hang.
TEST_F(ParkTest, ADescriptorThatEpollRefusesContinuesAtOnce) {
    char path[] = "/tmp/loxpp-park-XXXXXX";
    int file = ::mkstemp(path);
    ASSERT_GE(file, 0);
    g_parkFd = file;
    VM vm;
    vm.runtime().defineNative("park_fd", parkFdNative, 0);
    ASSERT_EQ(vm.interpret(R"(
        var me = task.self();
        var log = "";
        fun t() { park_fd(); log = log + "woke;"; task.send(me, 1); }
        task.spawn(t);
        task.receive();
    )"),
              InterpretResult::OK);
    ::close(file);
    ::unlink(path);
    EXPECT_EQ(globalString(vm, "log"), "woke;");
}

namespace {

struct ThreadSample {
    long ticks{-1};
    char state{'?'};
};

// The CPU time in clock ticks and the state of the thread named `name`, from
// /proc/self/task/<tid>/stat.
ThreadSample sampleThread(const std::string& name) {
    ThreadSample out;
    DIR* dir = opendir("/proc/self/task");
    if (dir == nullptr) {
        return out;
    }
    while (dirent* entry = readdir(dir)) {
        std::string tid = entry->d_name;
        if (tid == "." || tid == "..") {
            continue;
        }
        std::ifstream comm("/proc/self/task/" + tid + "/comm");
        std::string threadName;
        std::getline(comm, threadName);
        if (threadName != name) {
            continue;
        }
        std::ifstream statFile("/proc/self/task/" + tid + "/stat");
        std::string stat((std::istreambuf_iterator<char>(statFile)),
                         std::istreambuf_iterator<char>());
        std::istringstream rest(stat.substr(stat.rfind(')') + 2));
        std::vector<std::string> fields;
        std::string field;
        while (rest >> field) {
            fields.push_back(field);
        }
        if (fields.size() > 12) {
            out.state = fields[0][0];
            out.ticks = std::stol(fields[11]) + std::stol(fields[12]);
        }
        break;
    }
    closedir(dir);
    return out;
}

// True when the thread named `name` shows a quiet window: a window of 100 ms
// with at most one clock tick of CPU time that ends with the thread in the
// sleep state. The thread may start slowly under load, and a start in a debug
// build or under GC stress uses CPU for a while, so the windows run until one
// is quiet or `maxMs` pass. A thread that spins is busy in every window and
// is never quiet.
bool seesQuietWindow(const std::string& name, int maxMs) {
    ThreadSample last = sampleThread(name);
    for (int waited = 0; waited < maxMs; waited += 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ThreadSample now = sampleThread(name);
        if (last.ticks >= 0 && now.ticks >= 0 && now.ticks - last.ticks <= 1 &&
            now.state == 'S') {
            return true;
        }
        last = now;
    }
    return false;
}

// Runs `program`, which keeps the worker "loxpp-w1" asleep for about 1.4
// seconds, and looks for a quiet window of that worker meanwhile.
void expectWorkerSleepsWithoutSpinning(const std::string& program) {
    bool quiet = false;
    std::thread sampler([&] { quiet = seesQuietWindow("loxpp-w1", 1300); });
    VM vm;
    InterpretResult result = vm.interpret(program);
    sampler.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "late");
    EXPECT_TRUE(quiet) << "the worker used CPU in every window";
}

} // namespace

// A sleep where the task cannot suspend blocks the worker in the kernel: no
// CPU use, and the thread is in the sleep state.
TEST_F(ParkTest, ABlockingSleepDoesNotSpin) {
    expectWorkerSleepsWithoutSpinning(R"(
        class Slow { __str__() { sleep(1.5); return "late"; } }
        fun slow(main) { task.send(main, str(Slow())); }
        task.spawn_worker(slow, task.self());
        var got = task.receive();
    )");
}

// A sleep that parks leaves the worker in the poller, again without CPU use.
TEST_F(ParkTest, AParkedSleepDoesNotSpin) {
    expectWorkerSleepsWithoutSpinning(R"(
        fun slow(main) { sleep(1.5); task.send(main, "late"); }
        task.spawn_worker(slow, task.self());
        var got = task.receive();
    )");
}

// C7: a worker with a parked task can continue, so the group is not
// deadlocked while the main task waits in receive. The parked task is on a
// worker that is not the main one.
TEST_F(ParkTest, AWorkerWithAParkedTaskIsNotCountedIdle) {
    VM vm;
    testing::internal::CaptureStderr();
    InterpretResult result = vm.interpret(R"(
        fun slow(main) { sleep(0.4); task.send(main, "late"); }
        task.spawn_worker(slow, task.self());
        var got = task.receive();
    )");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, InterpretResult::OK) << err;
    EXPECT_EQ(err.find("Deadlock"), std::string::npos) << err;
    EXPECT_EQ(globalString(vm, "got"), "late");
}

// The deadlock report names the tasks that wait in receive. A parked task is
// not one of them, and it ends the report's cause: the program is deadlocked
// only when the sleep is over.
TEST_F(ParkTest, TheDeadlockReportDoesNotNameAParkedTask) {
    VM vm;
    testing::internal::CaptureStderr();
    InterpretResult result = vm.interpret(R"(
        fun napper() { sleep(0.3); }
        fun waiter() { task.receive(); }
        task.spawn(napper);
        task.spawn(waiter);
        task.receive();
    )");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, InterpretResult::RUNTIME_ERROR);
    EXPECT_NE(err.find("Deadlock: every task waits in receive.\n"),
              std::string::npos)
        << err;
    EXPECT_NE(err.find("task waiter on worker 0 waits in receive"),
              std::string::npos)
        << err;
    EXPECT_EQ(err.find("napper"), std::string::npos) << err;
}

// In a session the main task outlives a line that ended with a fault. If it
// was parked then, its wait ends with the line: the next line reports a real
// deadlock at once and does not wait for the old timer.
TEST_F(ParkTest, ASessionMainTaskThatWasParkedLeavesNoWaitBehind) {
    VMTestHarness h;
    h.setSession(true);
    ASSERT_EQ(h.run("fun boom() { sleep(0.1); var x = nil; x.foo(); }"),
              InterpretResult::OK);
    testing::internal::CaptureStderr();
    ASSERT_EQ(h.run("task.spawn(boom); sleep(8);"),
              InterpretResult::RUNTIME_ERROR);
    testing::internal::GetCapturedStderr();
    const auto start = PClock::now();
    testing::internal::CaptureStderr();
    InterpretResult result = h.run("task.receive();");
    std::string err = testing::internal::GetCapturedStderr();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        PClock::now() - start)
                        .count();
    h.endSession();
    EXPECT_EQ(result, InterpretResult::RUNTIME_ERROR);
    EXPECT_NE(err.find("Deadlock"), std::string::npos) << err;
    EXPECT_LT(ms, 3000);
}

// A sleep of zero, of a negative duration, and of a fraction of a nanosecond
// that rounds to zero return at once: no other task runs, and no park makes
// a poller.
TEST_F(ParkTest, ASleepWithNoDurationDoesNotPark) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        var me = task.self();
        var log = "";
        fun a() {
            sleep(0);
            log = log + "a;";
            sleep(-1);
            log = log + "a;";
            sleep(-0.5);
            log = log + "a;";
            task.send(me, 1);
        }
        fun b() { log = log + "b;"; task.send(me, 1); }
        task.spawn(a);
        task.spawn(b);
        task.receive();
        task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "a;a;a;b;");
    EXPECT_FALSE(vm.runtime().hasPoller());
}

// A very long sleep waits and does not overflow the clock or end early. The
// task would sleep for the rest of the clock's range: the main task ends
// first, and the sleeper never woke.
TEST_F(ParkTest, AVeryLongSleepDoesNotEndEarly) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        var woke = 0;
        fun forever(seconds) { sleep(seconds); woke = woke + 1; }
        fun power(n) {
            var x = 1;
            for (var i = 0; i < n; i = i + 1) { x = x * 1000000; }
            return x;
        }
        fun huge() { forever(power(5)); }
        fun big() { forever(power(2) * 10000); }
        fun nearLimit() { forever(9200000000); }
        fun infinite() { forever(power(60)); }
        task.spawn(huge);
        task.spawn(big);
        task.spawn(nearLimit);
        task.spawn(infinite);
        sleep(0.3);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "woke"), "0");
}
