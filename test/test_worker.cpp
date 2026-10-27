// test_worker.cpp — workers on OS threads (spec/04-semantics.md, Workers and
// Tasks; docs/decisions/concurrency-model-decision.md).
//
// The spec probes 73 to 76 and 82 to 84 pin the observable rules. These tests
// look inside: the holder count of a message (MemoryManager::holderCount),
// intern on receive, the dead heap of a worker that ended, the chain of a
// message that holds a message of a third worker, the deadlock report, an
// idle worker's CPU use, the cost of a send, and a long exchange of messages.
// The `_gc` twin runs the suite under LOXPP_STRESS_GC, which collects on
// every allocation on every worker.

#include "memory_manager.h"
#include "objects.h"
#include "thread_sampler.h"
#include "vm.h"
#include "worker.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

Obj* globalObj(const VM& vm, const std::string& name) {
    auto v = vm.getGlobal(name);
    EXPECT_TRUE(v.has_value()) << name;
    if (!v.has_value() || !isObj(*v)) {
        ADD_FAILURE() << name << " is not an object";
        return nullptr;
    }
    return as<Obj*>(*v);
}

std::string globalString(const VM& vm, const std::string& name) {
    auto v = vm.getGlobal(name);
    if (!v.has_value()) {
        return "<undefined>";
    }
    return stringify(*v);
}

bool stressGC() {
    const char* v = std::getenv("LOXPP_STRESS_GC");
    return v != nullptr && v[0] != '\0';
}

// Waits on the main task until the worker of `w` has ended: a send to it
// then raises DeadTaskError (spec/04-semantics.md, task.send).
const char* kAwaitEnd = R"(
    fun awaitEnd(w) {
        var ended = false;
        while (!ended) {
            try {
                task.send(w, nil);
                sleep(0.001);
            } catch (e) {
                ended = true;
            }
        }
    }
)";

// `body` after kAwaitEnd, with each GARBAGE replaced by an allocation count
// that passes the first collection threshold. Under GC stress every
// allocation collects, so a few suffice. A top-level var would not do: a
// new worker does not run it.
std::string withPrelude(const std::string& body) {
    std::string out = std::string(kAwaitEnd) + body;
    const std::string count = stressGC() ? "20" : "20000";
    for (std::size_t at = out.find("GARBAGE"); at != std::string::npos;
         at = out.find("GARBAGE", at)) {
        out.replace(at, 7, count);
    }
    return out;
}

} // namespace

class WorkerTest : public ::testing::Test {};

// A string that the receiver has already, as a literal of its source, comes
// in as the receiver's own object: nothing of the sender is held.
TEST_F(WorkerTest, ReceivedStringWithLocalCharsIsTheLocalObject) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        fun entry(main) {
            task.send(main, "ab" + "c");
        }
        var local = "abc";
        task.spawn_worker(entry, task.self());
        var got = task.receive();
        var same = got == local;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "same"), "true");
    Obj* got = globalObj(vm, "got");
    Obj* local = globalObj(vm, "local");
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got, local);
    EXPECT_EQ(got->heapId, vm.runtime().memoryManager().heapId());
}

// The same characters sent from two workers: the first string becomes the
// receiver's interned one, and the second resolves to it, so == is a pointer
// compare and the first sender's string has one holder, the receiver.
TEST_F(WorkerTest, SameStringFromTwoWorkersIsOneObject) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        fun entry(main) {
            task.send(main, "x" + "yz");
        }
        task.spawn_worker(entry, task.self());
        var first = task.receive();
        task.spawn_worker(entry, task.self());
        var second = task.receive();
        var same = first == second;
        var built = "xy" + "z";
        var sameAsBuilt = built == first;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "same"), "true");
    EXPECT_EQ(globalString(vm, "sameAsBuilt"), "true");
    Obj* first = globalObj(vm, "first");
    Obj* second = globalObj(vm, "second");
    Obj* built = globalObj(vm, "built");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
    // makeString found the received string in the intern table.
    EXPECT_EQ(first, built);
    EXPECT_NE(first->heapId, vm.runtime().memoryManager().heapId());
    EXPECT_EQ(heapOf(first)->holderCount(first), 1U);
}

// Duplicate sends: the same object sent twice to one worker, and the same
// string sent twice, leave one count for the one holder.
TEST_F(WorkerTest, DuplicateSendsCountOnce) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        enum Box { Of(v) }
        fun entry(main) {
            var e = Box.Of(1);
            var s = "dup" + "licate";
            task.send(main, e);
            task.send(main, e);
            task.send(main, s);
            task.send(main, s);
        }
        task.spawn_worker(entry, task.self());
        var e1 = task.receive();
        var e2 = task.receive();
        var s1 = task.receive();
        var s2 = task.receive();
    )"),
              InterpretResult::OK);
    Obj* e1 = globalObj(vm, "e1");
    Obj* e2 = globalObj(vm, "e2");
    Obj* s1 = globalObj(vm, "s1");
    Obj* s2 = globalObj(vm, "s2");
    ASSERT_NE(e1, nullptr);
    ASSERT_NE(s1, nullptr);
    EXPECT_EQ(e1, e2);
    EXPECT_EQ(s1, s2);
    EXPECT_EQ(heapOf(e1)->holderCount(e1), 1U);
    EXPECT_EQ(heapOf(s1)->holderCount(s1), 1U);
}

// The chain (docs/decisions/concurrency-model-decision.md, "The owner traces
// from a held object"): A receives F from C, builds E around F, sends E to
// the main task (B), and drops E. A and C then collect and end. The main
// task still reads F through E, because A traces from E while B holds it,
// and so keeps its hold on F.
TEST_F(WorkerTest, AMessageKeepsTheMessageOfAThirdWorker) {
    VM vm;
    std::string program = withPrelude(R"(
        enum Box { Of(v) }
        fun garbage() {
            var junk = [];
            for (var i = 0; i < GARBAGE; i = i + 1) {
                junk.append(str(i) + "...............................");
            }
        }
        fun c_entry(a) {
            task.send(a, Box.Of("F" + "!"));
            garbage();
        }
        fun a_entry(main) {
            var c = task.spawn_worker(c_entry, task.self());
            var f = task.receive();
            task.send(main, Box.Of(f));
            f = nil;
            garbage();
            awaitEnd(c);
        }
        var a = task.spawn_worker(a_entry, task.self());
        var e = task.receive();
        awaitEnd(a);
        garbage();
        var f = match e {
            case Box.Of(inner) => inner
            case _ => nil
        };
        var s = match f {
            case Box.Of(v) => v
            case _ => nil
        };
        var text = s + "";
        // A string of another heap is == to a local one with its characters,
        // also as a map key.
        var equal = s == "F!";
        var keys = {};
        keys["F!"] = "hit";
        var lookup = keys[s];
    )");
    ASSERT_EQ(vm.interpret(program), InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "text"), "F!");
    EXPECT_EQ(globalString(vm, "equal"), "true");
    EXPECT_EQ(globalString(vm, "lookup"), "hit");
    Obj* f = globalObj(vm, "f");
    ASSERT_NE(f, nullptr);
    // A is dead: its dead heap keeps E for the main task and, through E,
    // its own hold on F. The main task holds F too now, as a value it read.
    EXPECT_GE(heapOf(f)->holderCount(f), 1U);
}

// The receiver keeps only a field F of a received E and drops E. Its
// collection reaches F, which it never registered, and holds it before it
// releases E; then the owner collects, and F stays.
TEST_F(WorkerTest, AFieldKeptWithoutItsMessageIsHeldOnItsOwn) {
    VM vm;
    std::string program = withPrelude(R"(
        enum Box { Of(v) }
        fun garbage() {
            var junk = [];
            for (var i = 0; i < GARBAGE; i = i + 1) {
                junk.append(str(i) + "...............................");
            }
        }
        fun owner(main) {
            var e = Box.Of("fi" + "eld");
            task.send(main, e);
            e = nil;
            task.receive();
            garbage();
            task.send(main, "collected");
        }
        var w = task.spawn_worker(owner, task.self());
        var e = task.receive();
        var f = match e {
            case Box.Of(v) => v
            case _ => nil
        };
        e = nil;
        garbage();
        task.send(w, "go");
        task.receive();
        awaitEnd(w);
        var text = f + "!";
    )");
    ASSERT_EQ(vm.interpret(program), InterpretResult::OK);
    Obj* f = globalObj(vm, "f");
    ASSERT_NE(f, nullptr);
    ASSERT_NE(heapOf(f), nullptr) << "the owner's heap was freed under F";
    EXPECT_EQ(heapOf(f)->holderCount(f), 1U);
    EXPECT_EQ(globalString(vm, "text"), "field!");
}

// A worker sends a message and ends while the receiver holds it. The
// receiver reads every field afterwards. The dead heap is freed when the
// receiver drops the message.
TEST_F(WorkerTest, DeadHeapLivesWhileItsMessageIsHeld) {
    VM vm;
    std::string program = kAwaitEnd;
    program += R"(
        enum Tree { Leaf(v) Node(l, r) }
        fun entry(main) {
            var t = Tree.Node(Tree.Leaf("left" + "!"), Tree.Node(Tree.Leaf(2), Tree.Leaf("r" + "r")));
            task.send(main, t);
        }
        var w = task.spawn_worker(entry, task.self());
        var held = task.receive();
        awaitEnd(w);
        fun sum(t) {
            return match t {
                case Tree.Leaf(v) => str(v)
                case Tree.Node(l, r) => sum(l) + "," + sum(r)
            };
        }
        var text = sum(held);
    )";
    ASSERT_EQ(vm.interpret(program), InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "text"), "left!,2,rr");
    Obj* held = globalObj(vm, "held");
    ASSERT_NE(held, nullptr);
    std::uint16_t deadId = held->heapId;
    MemoryManager* dead = heapById(deadId);
    ASSERT_NE(dead, nullptr);
    EXPECT_EQ(dead->holderCount(held), 1U);

    // Drop the message and the handle, then collect: the main worker
    // releases its last holds on the heap. The heap is freed by whichever
    // comes last, this release or the end of the worker's thread, which can
    // still be on its way out after its task ended.
    ASSERT_EQ(vm.interpret("held = nil; w = nil;"), InterpretResult::OK);
    vm.runtime().memoryManager().collectGarbage();
    for (int i = 0; i < 500 && heapById(deadId) != nullptr; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(heapById(deadId), nullptr);
}

// The deadlock report names every task that waits, on every worker. The
// fault is fatal: the try does not catch it.
TEST_F(WorkerTest, DeadlockNamesTheWaitingTasksOfEveryWorker) {
    VM vm;
    testing::internal::CaptureStderr();
    InterpretResult result = vm.interpret(R"(
        fun waiter() {
            task.receive();
        }
        fun far(m) {
            task.receive();
        }
        task.spawn(waiter);
        task.spawn_worker(far, nil);
        var caught = false;
        try {
            task.receive();
        } catch (e) {
            caught = true;
        }
    )");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, InterpretResult::RUNTIME_ERROR);
    EXPECT_NE(err.find("Deadlock: every task waits in receive.\n"),
              std::string::npos)
        << err;
    EXPECT_NE(err.find("  task script on worker 0 waits in receive\n"),
              std::string::npos)
        << err;
    EXPECT_NE(err.find("  task waiter on worker 0 waits in receive\n"),
              std::string::npos)
        << err;
    EXPECT_NE(err.find("  task far on worker 1 waits in receive\n"),
              std::string::npos)
        << err;
    EXPECT_EQ(globalString(vm, "caught"), "false");
}

// No deadlock while one worker still runs: the main task waits, the other
// worker sleeps, then sends.
TEST_F(WorkerTest, NoDeadlockWhileAWorkerRuns) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        fun slow(main) {
            sleep(0.2);
            task.send(main, "late");
        }
        task.spawn_worker(slow, task.self());
        var got = task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "late");
}

// An idle worker sleeps on its condition variable: while the main task waits
// in a blocking call, the worker that waits in receive uses no CPU, and its
// thread is in the sleep state.
TEST_F(WorkerTest, AnIdleWorkerUsesNoCpu) {
    bool quiet = false;
    std::thread sampler([&] { quiet = seesQuietWindow("loxpp-w1", 1300); });
    VM vm;
    InterpretResult result = vm.interpret(R"(
        fun idle(main) {
            var m = task.receive();
            task.send(main, m);
        }
        var w = task.spawn_worker(idle, task.self());
        sleep(1.5);
        task.send(w, "wake");
        var got = task.receive();
    )");
    sampler.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "wake");
    EXPECT_TRUE(quiet) << "the worker used CPU in every window";
}

// --- The poller of a worker --------------------------------------------------
//
// These tests register the wait from the host, through Runtime::poller(), and
// handle it with Runtime::setPollHandler().

namespace {

using PClock = Poller::Clock;

long msSince(PClock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(PClock::now() -
                                                                 start)
        .count();
}

} // namespace

// A program that never parks makes no poller, and opens no descriptor.
TEST_F(WorkerTest, AWorkerThatNeverParksMakesNoPoller) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        fun far(main) { sleep(0.01); task.send(main, "x"); }
        task.spawn_worker(far, task.self());
        var got = task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_FALSE(vm.runtime().hasPoller());
}

// The main task waits in receive while its worker sleeps in the poller, on a
// timer that is far away. A send from another worker must wake it at once. If
// the sender does not write the wakeup descriptor, the worker sleeps until the
// timer.
TEST_F(WorkerTest, SendWakesAWorkerThatSleepsInItsPoller) {
    VM vm;
    vm.runtime().poller().waitUntil(PClock::now() + std::chrono::seconds(8));
    const auto start = PClock::now();
    ASSERT_EQ(vm.interpret(R"(
        fun far(main) { sleep(0.1); task.send(main, "hi"); }
        task.spawn_worker(far, task.self());
        var got = task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "hi");
    EXPECT_LT(msSince(start), 4000);
}

// The main worker holds a timer, so it is busy, and the main task waits in
// receive. The timer posts the message: no DeadlockError may come first.
TEST_F(WorkerTest, NoFalseDeadlockWhileAWaitIsRegistered) {
    VM vm;
    int fired = 0;
    Runtime& rt = vm.runtime();
    rt.poller().waitUntil(PClock::now() + std::chrono::milliseconds(300));
    rt.setPollHandler([&](const std::vector<Poller::Token>&) {
        ++fired;
        // C7: the worker holds a wait, so it never counted itself idle.
        // This is the test that ties waitForWork to WaitReason::Poller.
        Inbox& inbox = rt.memoryManager().inbox();
        EXPECT_FALSE(inbox.idle);
        EXPECT_EQ(inbox.group->busy.load(), 1);
        auto me = vm.getGlobal("me");
        ASSERT_TRUE(me.has_value());
        ASSERT_TRUE(postEnvelope(
            rt.memoryManager().inbox(),
            Envelope{static_cast<ObjTask*>(as<Obj*>(*me)), Value{Number{7}}}));
    });
    testing::internal::CaptureStderr();
    InterpretResult result = vm.interpret(R"(
        var me = task.self();
        var got = task.receive();
    )");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, InterpretResult::OK) << err;
    EXPECT_EQ(err.find("Deadlock"), std::string::npos) << err;
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(globalString(vm, "got"), "7");
}

// The runtime clears the pointers that senders use before the poller is
// destroyed, and the inbox outlives the runtime while a message is held.
TEST_F(WorkerTest, RuntimeClearsItsPollerFromTheInboxOnTeardown) {
    MemoryManager* heap = nullptr;
    Value held;
    {
        VM vm;
        ASSERT_EQ(vm.interpret("var s = [1];"), InterpretResult::OK);
        heap = &vm.runtime().memoryManager();
        vm.runtime().poller();
        {
            std::lock_guard<std::mutex> lock(heap->inbox().mutex);
            EXPECT_NE(heap->inbox().poller, nullptr);
            heap->inbox().polling = true;
        }
        Obj* obj = globalObj(vm, "s");
        ASSERT_NE(obj, nullptr);
        heap->hold(obj);
        held = Value{obj};
    }
    {
        std::lock_guard<std::mutex> lock(heap->inbox().mutex);
        EXPECT_EQ(heap->inbox().poller, nullptr);
        EXPECT_FALSE(heap->inbox().polling);
    }
    MemoryManager::releaseValue(held);
}

// When the registered wait ends and nothing can send, the deadlock is real.
TEST_F(WorkerTest, DeadlockIsReportedWhenTheLastWaitEnds) {
    VM vm;
    int fired = 0;
    const auto start = PClock::now();
    vm.runtime().poller().waitUntil(PClock::now() +
                                    std::chrono::milliseconds(200));
    vm.runtime().setPollHandler(
        [&](const std::vector<Poller::Token>&) { ++fired; });
    testing::internal::CaptureStderr();
    InterpretResult result = vm.interpret("var got = task.receive();");
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(result, InterpretResult::RUNTIME_ERROR);
    EXPECT_NE(err.find("Deadlock: every task waits in receive.\n"),
              std::string::npos)
        << err;
    EXPECT_EQ(fired, 1);
    EXPECT_GE(msSince(start), 190);
}

// Two workers exchange many messages, enum values with a string field, both
// ways, under collection on both sides. The `_gc` twin collects on every
// allocation. A count off by one in either direction frees a message that
// the other side still reads, or leaks the sender's heap.
TEST_F(WorkerTest, TwoWorkersExchangeManyMessages) {
    VM vm;
    const int count = stressGC() ? 5000 : 50000;
    std::string program = "var count = " + std::to_string(count) + ";";
    program += R"(
        enum Msg { Ping(n, text) Pong(n, text) }
        fun echo(main) {
            var running = true;
            while (running) {
                var m = task.receive();
                if (m == "stop") {
                    running = false;
                } else {
                    task.send(main, match m {
                        case Msg.Ping(n, text) => Msg.Pong(n, text + "!")
                        case _ => nil
                    });
                }
            }
        }
        var w = task.spawn_worker(echo, task.self());
        var ok = 0;
        for (var i = 0; i < count; i = i + 1) {
            task.send(w, Msg.Ping(i, "p" + str(i % 7)));
            var r = task.receive();
            var good = match r {
                case Msg.Pong(n, text) => n == i and text == "p" + str(i % 7) + "!"
                case _ => false
            };
            if (good) ok = ok + 1;
        }
        task.send(w, "stop");
    )";
    ASSERT_EQ(vm.interpret(program), InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "ok"), std::to_string(count));
}

// The bytes allocated on the heap of a task. The caller reads a worker's heap
// only while that worker waits in receive, after a reply it sent: the reply
// orders the worker's allocations before the read.
Value heapBytesNative(int /*argc*/, Value* argv) {
    ObjTask* t = asObjTask(as<Obj*>(argv[0]));
    MemoryManager* heap = heapById(t->heapId);
    return from<Number>(static_cast<Number>(heap->bytesAllocated));
}

// A send hands over the message without copying it. A copy would allocate in
// proportion to the message size, on the sending heap or, at the drain, on
// the receiving heap. The test counts the bytes allocated on both heaps over
// many sends and never reads a clock, so the load of the host cannot change
// the result. It does not see a copy into memory that no heap tracks.
TEST_F(WorkerTest, SendDoesNotAllocateInProportionToTheMessageSize) {
    if (stressGC()) {
        GTEST_SKIP() << "GC stress collects at every allocation";
    }
    VM vm;
    vm.runtime().defineNative("heap_bytes", heapBytesNative, 1);
    ASSERT_EQ(vm.interpret(R"(
        enum List { Empty Cons(head, tail) }
        fun build(n) {
            var l = List.Empty();
            for (var i = 0; i < n; i = i + 1) l = List.Cons(i, l);
            return l;
        }
        fun sink(main) {
            while (true) {
                var m = task.receive();
                while (m != "mark") m = task.receive();
                task.send(main, "marked");
            }
        }
        fun bytesPerSend(msg, count) {
            var w = task.spawn_worker(sink, task.self());
            task.send(w, "mark");
            task.receive();
            var sentBefore = heap_bytes(task.self());
            var receivedBefore = heap_bytes(w);
            for (var i = 0; i < count; i = i + 1) task.send(w, msg);
            var sentAfter = heap_bytes(task.self());
            task.send(w, "mark");
            task.receive();
            var receivedAfter = heap_bytes(w);
            return [(sentAfter - sentBefore) / count,
                    (receivedAfter - receivedBefore) / count];
        }
        var count = 300;
        var small = bytesPerSend(build(100), count);
        var large = bytesPerSend(build(1000), count);
        var sentSmall = small[0];
        var receivedSmall = small[1];
        var sentLarge = large[0];
        var receivedLarge = large[1];
    )"),
              InterpretResult::OK);

    // A copy of the smaller message is 100 objects, thousands of bytes. The
    // allowance of 100 bytes per send is far below that. A collection only
    // lowers the count, so it cannot break the bound.
    for (const char* name :
         {"sentSmall", "sentLarge", "receivedSmall", "receivedLarge"}) {
        auto v = vm.getGlobal(name);
        ASSERT_TRUE(v.has_value() && is<Number>(*v)) << name;
        EXPECT_LT(as<Number>(*v), 100.0)
            << name << " allocated " << as<Number>(*v) << " bytes per send";
    }
}

// A worker's uncaught fault ends the program with status 70 after the
// report, also while the main task runs.
TEST(WorkerDeathTest, AnUncaughtFaultOnAWorkerEndsTheProgram) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            VM vm;
            vm.interpret(R"(
                fun bad(m) {
                    return m + 1;
                }
                task.spawn_worker(bad, "text");
                sleep(10);
                print "main went on";
            )");
            std::exit(0);
        },
        testing::ExitedWithCode(70), "Operands must be two numbers");
}

// exit(code) on a worker ends the program with that code.
TEST(WorkerDeathTest, ExitOnAWorkerEndsTheProgram) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            VM vm;
            vm.interpret(R"(
                fun quit(m) {
                    exit(3);
                }
                task.spawn_worker(quit, nil);
                sleep(10);
            )");
            std::exit(0);
        },
        testing::ExitedWithCode(3), "");
}

// A fault while the new worker defines a declaration is a fault of that
// worker's task (spec/04-semantics.md, task.spawn_worker): the program ends.
TEST(WorkerDeathTest, AFaultInTheDeclarationsOfAWorkerEndsTheProgram) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            VM vm;
            vm.interpret(R"(
                var Base = nil;
                class Base2 {}
                Base = Base2;
                class D < Base {}
                fun entry(m) {
                    print "entry ran";
                }
                task.spawn_worker(entry, nil);
                sleep(10);
            )");
            std::exit(0);
        },
        testing::ExitedWithCode(70), "Undefined variable");
}
