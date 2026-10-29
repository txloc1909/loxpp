// test_socket_park.cpp — the read side of a socket on a task that parks.
//
// The spec probes 261 to 265, 278, 280, 281, 284 and 285 pin what a program
// sees. These tests look at what a probe cannot see well: a call that waits
// many times, the bytes at each chunk boundary, the level-triggered re-arm, a
// close while two tasks wait, a descriptor number that comes back, the roots
// of a parked call, a signal in the blocking wait, and the descriptor that the
// end of the program closes. A feeder task writes in pieces with a sleep
// between them, on the same worker as the reader, so every piece makes the
// reader park again. The `_gc` twin runs the suite under LOXPP_STRESS_GC.

#include "container_objects.h"
#include "poller.h"
#include "test_harness.h"
#include "vm.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {

std::string globalString(const VM& vm, const std::string& name) {
    auto v = vm.getGlobal(name);
    return v.has_value() ? stringify(*v) : "<undefined>";
}

// A listener, a client and the accepted connection, and a feeder task that
// runs `feed` on the client side. Every test program starts with this.
const char* const kPair = R"(
    var srv = listen("127.0.0.1", 0);
    var peer = connect("127.0.0.1", srv.port());
    var conn = srv.accept();
    var me = task.self();
)";

} // namespace

class SocketParkTest : public ::testing::Test {
  protected:
    // Runs the pair prologue and `body`; the test reads the globals.
    static InterpretResult run(VM& vm, const std::string& body) {
        return vm.interpret(std::string(kPair) + body);
    }
};

// A line that comes in three chunks: the reader parks at each chunk, and the
// bytes of the three joined, in order, are the line.
TEST_F(SocketParkTest, ALineInThreeChunks) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var got = "none";
        var ticks = 0;
        fun reader() { got = conn.readline(); task.send(me, 1); }
        fun feeder() {
            peer.write("abc"); sleep(0.05); ticks = ticks + 1;
            peer.write("def"); sleep(0.05); ticks = ticks + 1;
            peer.write("ghi\nrest"); task.send(me, 1);
        }
        task.spawn(reader);
        task.spawn(feeder);
        task.receive(); task.receive();
        var rest = conn.read_bytes(4);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "abcdefghi");
    EXPECT_EQ(globalString(vm, "rest"), "rest");
    EXPECT_EQ(globalString(vm, "ticks"), "2");
}

// A line split between "\r" and "\n" keeps its "\r": readline cuts at "\n"
// only.
TEST_F(SocketParkTest, AReturnStaysInTheLine) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var got = "none";
        fun reader() { got = conn.readline(); task.send(me, 1); }
        fun feeder() {
            peer.write("one\r"); sleep(0.05);
            peer.write("\ntwo\n"); task.send(me, 1);
        }
        task.spawn(reader);
        task.spawn(feeder);
        task.receive(); task.receive();
        var next = conn.readline();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "one\r");
    EXPECT_EQ(globalString(vm, "next"), "two");
}

// read_bytes(n) waits for n bytes across chunks and returns exactly n; the
// bytes past n stay for the next read.
TEST_F(SocketParkTest, ReadBytesAcrossChunks) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var got = "none";
        fun reader() { got = conn.read_bytes(7); task.send(me, 1); }
        fun feeder() {
            peer.write("ab"); sleep(0.05);
            peer.write("cde"); sleep(0.05);
            peer.write("fghij"); task.send(me, 1);
        }
        task.spawn(reader);
        task.spawn(feeder);
        task.receive(); task.receive();
        var tail = conn.read_bytes(3);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "abcdefg");
    EXPECT_EQ(globalString(vm, "tail"), "hij");
}

// The end of the stream: readline gives nil, read gives "", read_bytes gives
// the bytes that came, and readlines gives the lines. Each call waits for the
// close of the peer.
TEST_F(SocketParkTest, EndOfStream) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var a = "unset"; var b = "unset"; var c = "unset"; var d = "unset";
        fun closer() { sleep(0.05); peer.close(); task.send(me, 1); }
        fun r1() { a = conn.readline(); task.send(me, 1); }
        task.spawn(r1);
        task.spawn(closer);
        task.receive(); task.receive();
        b = conn.read();
        c = conn.read_bytes(5);
        d = conn.readlines();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "a"), "nil");
    EXPECT_EQ(globalString(vm, "b"), "");
    EXPECT_EQ(globalString(vm, "c"), "");
    EXPECT_EQ(globalString(vm, "d"), "[]");
}

TEST_F(SocketParkTest, ReadBytesAndReadlinesAtEndAfterPartialData) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var got = "none"; var lines = "none";
        fun reader() {
            got = conn.read_bytes(10);
            lines = conn.readlines();
            task.send(me, 1);
        }
        fun feeder() {
            peer.write("abc"); sleep(0.05);
            peer.close(); task.send(me, 1);
        }
        task.spawn(reader);
        task.spawn(feeder);
        task.receive(); task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "abc");
    EXPECT_EQ(globalString(vm, "lines"), "[]");
}

// A read of zero bytes, and a read that the input buffer already satisfies,
// return at once and park nothing: no poller exists afterwards.
TEST_F(SocketParkTest, ACallWithNoWaitDoesNotPark) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        peer.write("x\ny\nz");
        var first = conn.readline();
        var zero = conn.read_bytes(0);
        var second = conn.readline();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "first"), "x");
    EXPECT_EQ(globalString(vm, "zero"), "");
    EXPECT_EQ(globalString(vm, "second"), "y");
    EXPECT_FALSE(vm.runtime().hasPoller());
}

// Level-triggered re-arm: two tasks wait on one socket. One chunk of data
// wakes both; the first takes it, and the second finds nothing and waits
// again until the next chunk comes.
TEST_F(SocketParkTest, ASecondReaderWaitsAgainWhenTheFirstTookTheData) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var a = "none"; var b = "none";
        fun r1() { a = conn.readline(); task.send(me, 1); }
        fun r2() { b = conn.readline(); task.send(me, 1); }
        fun feeder() {
            peer.writeline("one"); sleep(0.05);
            peer.writeline("two"); task.send(me, 1);
        }
        task.spawn(r1);
        task.spawn(r2);
        task.spawn(feeder);
        task.receive(); task.receive(); task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "a"), "one");
    EXPECT_EQ(globalString(vm, "b"), "two");
}

// A close while two tasks wait on the socket fails both calls with
// SocketClosedError, and the message names each call.
TEST_F(SocketParkTest, ACloseFailsEveryParkedCallWithItsOwnName) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var a = "none"; var b = "none"; var c = "none";
        fun r1() {
            try { conn.readline(); } catch (e) { a = e.kind + ": " + e.message; }
            task.send(me, 1);
        }
        fun r2() {
            try { conn.read_bytes(3); } catch (e) { b = e.kind + ": " + e.message; }
            task.send(me, 1);
        }
        fun r3() {
            try { srv.accept(); srv.accept(); } catch (e) { c = e.kind + ": " + e.message; }
            task.send(me, 1);
        }
        fun closer() { conn.close(); srv.close(); task.send(me, 1); }
        task.spawn(r1);
        task.spawn(r2);
        task.spawn(r3);
        task.spawn(closer);
        task.receive(); task.receive(); task.receive(); task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "a"),
              "SocketClosedError: Cannot call 'readline' on a closed socket.");
    EXPECT_EQ(
        globalString(vm, "b"),
        "SocketClosedError: Cannot call 'read_bytes' on a closed socket.");
    EXPECT_EQ(globalString(vm, "c"),
              "SocketClosedError: Cannot call 'accept' on a closed server.");
}

// A close returns the descriptor number to the kernel. A new connection that
// gets the same number must not meet the wait of the closed one: the new
// reader gets its own bytes, and the old reader got its fault and nothing
// else.
TEST_F(SocketParkTest, ADescriptorNumberThatComesBackMeetsNoStaleWait) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var old = "none"; var fresh = "none";
        var other = nil;
        fun r1() {
            try { conn.readline(); old = "line"; }
            catch (e) { old = e.kind; }
            task.send(me, 1);
        }
        fun r2() {
            other = connect("127.0.0.1", srv.port());
            var c2 = srv.accept();
            fresh = c2.readline();
            c2.close();
            task.send(me, 1);
        }
        fun closer() {
            conn.close();
            sleep(0.05);
            other.writeline("hello");
            task.send(me, 1);
        }
        task.spawn(r1);
        task.spawn(closer);
        task.receive();
        task.spawn(r2);
        task.receive(); task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "old"), "SocketClosedError");
    EXPECT_EQ(globalString(vm, "fresh"), "hello");
}

// The collector runs while the call waits. The socket has no other holder
// than the parked task, and the line is built from chunks that arrive during
// collections.
TEST_F(SocketParkTest, TheSocketOfAParkedCallStaysAliveAcrossCollections) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        var srv = listen("127.0.0.1", 0);
        var peer = connect("127.0.0.1", srv.port());
        var me = task.self();
        var got = "none";
        var junk = 0;
        fun reader() {
            got = srv.accept().readline();
            task.send(me, 1);
        }
        fun feeder() {
            var i = 0;
            while (i < 200) { var s = "x" + str(i); junk = junk + len(s); i = i + 1; }
            peer.writeline("alive");
            task.send(me, 1);
        }
        task.spawn(reader);
        task.spawn(feeder);
        task.receive(); task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "alive");
}

// A very long line: the socket keeps only the unread tail afterwards, also
// when the call waited.
TEST_F(SocketParkTest, ALongLineThatWaitedLeavesNoCapacityBehind) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        fun rep(t, n) { var r = ""; var i = 0; while (i < n) { r = r + t; i = i + 1; } return r; }
        var first = "none";
        fun reader() { first = conn.readline(); task.send(me, 1); }
        fun feeder() {
            peer.write(rep("0123456789", 5000)); sleep(0.05);
            peer.write(rep("0123456789", 5000) + "\nsmall\n"); task.send(me, 1);
        }
        task.spawn(reader);
        task.spawn(feeder);
        task.receive(); task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "first").size(), 100000u);
    auto v = vm.getGlobal("conn");
    ASSERT_TRUE(v.has_value() && isSocket(*v));
    ObjSocket* sock = asObjSocket(as<Obj*>(*v));
    EXPECT_LE(sock->inbuf.capacity(), 2u * 4096u);
    EXPECT_LE(sock->inbuf.size(), 4096u);
}

// Two readers wait on one socket and a partial line is already in the buffer.
// The first takes the line when its end arrives; the second must then find the
// next newline in the bytes that are in the buffer now, and not wait for more
// data because an offset from before the park points past it.
TEST_F(SocketParkTest, ASecondReaderScansTheBufferAsItIsNow) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var l1 = "none"; var l2 = "none"; var l2Early = "none";
        fun r1() { l1 = conn.readline(); task.send(me, 1); }
        fun r2() { l2 = conn.readline(); task.send(me, 1); }
        peer.write("xxxxx");
        task.spawn(r1);
        task.spawn(r2);
        sleep(0.1);
        peer.write("\nyy\nzz");
        sleep(0.2);
        l2Early = l2;
        peer.write("\n");
        task.receive(); task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "l1"), "xxxxx");
    EXPECT_EQ(globalString(vm, "l2Early"), "yy");
    EXPECT_EQ(globalString(vm, "l2"), "yy");
}

// As above, but the data after the first line holds a second newline already.
// An offset from before the park that points past the first one would return
// a line with a newline inside it.
TEST_F(SocketParkTest, ASecondReaderNeverReturnsALineWithANewline) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var l1 = "none"; var l2 = "none";
        fun r1() { l1 = conn.readline(); task.send(me, 1); }
        fun r2() { l2 = conn.readline(); task.send(me, 1); }
        peer.write("xxxxx");
        task.spawn(r1);
        task.spawn(r2);
        sleep(0.1);
        peer.write("\nyy\nzz\n");
        task.receive(); task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "l1"), "xxxxx");
    EXPECT_EQ(globalString(vm, "l2"), "yy");
}

// A close fails the parked calls in the order in which they began to wait,
// and the failed tasks join the run queue in that order.
TEST_F(SocketParkTest, ACloseFailsParkedCallsInTheOrderTheyBeganToWait) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var log = "";
        fun a() { try { conn.readline(); } catch (e) { log = log + "a"; } task.send(me, 1); }
        fun b() { try { conn.read_bytes(2); } catch (e) { log = log + "b"; } task.send(me, 1); }
        fun c() { try { conn.read(); } catch (e) { log = log + "c"; } task.send(me, 1); }
        fun d() { try { conn.readlines(); } catch (e) { log = log + "d"; } task.send(me, 1); }
        task.spawn(a); task.spawn(b); task.spawn(c); task.spawn(d);
        var y = yield nil;
        conn.close();
        task.receive(); task.receive(); task.receive(); task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "abcd");
}

// A call that waits again after a partial chunk began to wait at its first
// wait. When its Socket and the Socket of a later call become ready in one
// poll, the earlier call continues first. A new place at each wait would put
// it behind the later call.
TEST_F(SocketParkTest, ARearmedCallContinuesBeforeALaterCall) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        var other_out = connect("127.0.0.1", srv.port());
        var other_in = srv.accept();
        var log = "";
        fun a() { conn.readline(); log = log + "a"; task.send(me, 1); }
        fun b() { other_in.readline(); log = log + "b"; task.send(me, 1); }
        fun driver() {
            peer.write("x");
            sleep(0.05);
            peer.writeline("y");
            other_out.writeline("z");
            task.send(me, 1);
        }
        task.spawn(a); task.spawn(b); task.spawn(driver);
        task.receive(); task.receive(); task.receive();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "ab");
}

namespace {

// A listener on the host side of the process, for a test that needs a peer
// outside the VM: the peer can write after a delay and can see the descriptor
// close.
class HostListener {
  public:
    HostListener() {
        m_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
                  0);
        EXPECT_EQ(listen(m_fd, 4), 0);
        socklen_t len = sizeof(addr);
        getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
    }
    ~HostListener() { close(m_fd); }
    HostListener(const HostListener&) = delete;
    HostListener& operator=(const HostListener&) = delete;
    // The next connection, or -1 after 10 seconds. A signal does not end the
    // wait, and a program that never connects does not hang the test.
    int accept() const {
        const auto end =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < end) {
            struct pollfd pfd = {m_fd, POLLIN, 0};
            if (poll(&pfd, 1, 100) > 0) {
                int fd = ::accept(m_fd, nullptr, nullptr);
                if (fd >= 0 || errno != EINTR) {
                    return fd;
                }
            }
        }
        return -1;
    }
    int port() const { return m_port; }

  private:
    int m_fd{-1};
    int m_port{0};
};

} // namespace

// Where the task cannot suspend, the call blocks the worker and does not
// raise, also when a signal interrupts the wait. The handler has no
// SA_RESTART, so poll(2) and read(2) return EINTR. The shooter sends the
// signal to every thread of the process but its own.
TEST_F(SocketParkTest, ABlockingWaitSurvivesSignals) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = [](int) {};
    sigemptyset(&act.sa_mask);
    struct sigaction old;
    memset(&old, 0, sizeof(old));
    ASSERT_EQ(sigaction(SIGUSR1, &act, &old), 0);

    HostListener host;
    std::thread peer([&] {
        int fd = host.accept();
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const char* const first = "la";
        const char* const second = "te\n";
        send(fd, first, 2, MSG_NOSIGNAL);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        send(fd, second, 3, MSG_NOSIGNAL);
        close(fd);
    });
    std::atomic<bool> stop{false};
    std::thread shooter([&] {
        const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
        // The program connects first, and connect() does not retry on a
        // signal, so the signals start after the connection is open.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        while (!stop.load()) {
            DIR* dir = opendir("/proc/self/task");
            while (dir != nullptr) {
                dirent* ent = readdir(dir);
                if (ent == nullptr) {
                    break;
                }
                pid_t tid = static_cast<pid_t>(atoi(ent->d_name));
                if (tid > 0 && tid != self) {
                    syscall(SYS_tgkill, getpid(), tid, SIGUSR1);
                }
            }
            if (dir != nullptr) {
                closedir(dir);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    VM vm;
    InterpretResult result = vm.interpret("var c = connect(\"127.0.0.1\", " +
                                          std::to_string(host.port()) +
                                          R"();
        var got = "none";
        class Reads { __str__() { got = c.readline(); return "r"; } }
        var s = str(Reads());
        var after = c.readline();
    )");
    stop = true;
    shooter.join();
    peer.join();
    sigaction(SIGUSR1, &old, nullptr);
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "late");
    EXPECT_EQ(globalString(vm, "after"), "nil");
}

// The end of the program closes the descriptor of a parked call: the host
// peer sees the end of the stream, and the program has not waited for it.
TEST_F(SocketParkTest, TheEndOfTheProgramClosesTheDescriptorOfAParkedCall) {
    HostListener host;
    std::atomic<ssize_t> seen{-2};
    std::thread peer([&] {
        int fd = host.accept();
        struct pollfd pfd = {fd, POLLIN, 0};
        // 5 seconds: the close comes when the program ends, at once.
        if (poll(&pfd, 1, 5000) > 0) {
            char c;
            seen = recv(fd, &c, 1, 0);
        }
        close(fd);
    });
    VM vm;
    const auto start = std::chrono::steady_clock::now();
    InterpretResult result = vm.interpret("var c = connect(\"127.0.0.1\", " +
                                          std::to_string(host.port()) +
                                          R"();
        fun reader() { c.readline(); }
        task.spawn(reader);
        var y = yield nil;
    )");
    peer.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(seen.load(), 0) << "the peer did not see the end of the stream";
    EXPECT_LT(ms, 3000);
}

// A free port number: listen on port 0, read the number, close.
static int freePort() {
    HostListener probe;
    return probe.port();
}

// The read calls in every context where a task cannot suspend block the
// worker and do not raise: the ticker, a task of the same worker, runs after
// the read, and the read returns the bytes. The host writes the line after
// 150 ms.
static void expectReadBlocks(const std::string& context) {
    HostListener host;
    std::thread peer([&] {
        int fd = host.accept();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        send(fd, "ctx\n", 4, MSG_NOSIGNAL);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret("var c = connect(\"127.0.0.1\", " +
                                          std::to_string(host.port()) +
                                          R"();
        var log = ""; var got = "none";
        fun ticker() { log = log + "T"; }
        task.spawn(ticker);
    )" + context +
                                          R"(
        var y = yield nil;
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "ctx");
    EXPECT_EQ(globalString(vm, "log"), "RT");
}

TEST_F(SocketParkTest, AReadInStrBlocksTheWorker) {
    expectReadBlocks(R"(
        class A { __str__() { got = c.readline(); log = log + "R"; return "a"; } }
        var s = str(A());
    )");
}

TEST_F(SocketParkTest, AReadInAMapKeyHashBlocksTheWorker) {
    expectReadBlocks(R"(
        class K {
            __hash__() { got = c.readline(); log = log + "R"; return 1; }
            __eq__(o) { return true; }
        }
        var m = {};
        m[K()] = 1;
    )");
}

TEST_F(SocketParkTest, AReadInCallMethodBlocksTheWorker) {
    expectReadBlocks(R"(
        class M { run() { got = c.read_bytes(3); log = log + "R"; return 1; } }
        var r = callMethod(M(), "run");
    )");
}

TEST_F(SocketParkTest, AReadInADeferredCallBlocksTheWorker) {
    expectReadBlocks(R"(
        fun rd() { got = c.readline(); log = log + "R"; }
        fun f() { defer rd(); }
        f();
    )");
}

TEST_F(SocketParkTest, AReadInANestedCoroutineBlocksTheWorker) {
    expectReadBlocks(R"(
        fun inner() { got = c.readline(); log = log + "R"; return 1; }
        var co = coroutine.create(inner);
        class S { __str__() { co.resume(); return "s"; } }
        var s = str(S());
    )");
}

// accept in a nested coroutine blocks the worker until a host client
// connects, and does not raise.
TEST_F(SocketParkTest, AnAcceptInANestedCoroutineBlocksTheWorker) {
    const int port = freePort();
    std::thread client([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (int i = 0; i < 50; ++i) {
            int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(static_cast<uint16_t>(port));
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
                0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                close(fd);
                return;
            }
            close(fd);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    VM vm;
    InterpretResult result =
        vm.interpret("var srv = listen(\"127.0.0.1\", " + std::to_string(port) +
                     R"();
        var log = ""; var got = "none";
        fun ticker() { log = log + "T"; }
        task.spawn(ticker);
        fun inner() { got = type(srv.accept()); log = log + "R"; return 1; }
        var co = coroutine.create(inner);
        class S { __str__() { co.resume(); return "s"; } }
        var s = str(S());
        var y = yield nil;
    )");
    client.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "Socket");
    EXPECT_EQ(globalString(vm, "log"), "RT");
}

// A write of 16 MiB to a reader that starts late fills the connection. The
// write waits for room in the kernel and ends when the reader has drained
// everything.
TEST_F(SocketParkTest, ALargeWriteWaitsForRoomAndSendsEveryByte) {
    HostListener host;
    std::atomic<size_t> total{0};
    std::thread peer([&] {
        int fd = host.accept();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::string buf(65536, '\0');
        for (;;) {
            // A write that failed leaves the connection open; do not wait on
            // it.
            struct pollfd pfd = {fd, POLLIN, 0};
            if (poll(&pfd, 1, 5000) <= 0) {
                break;
            }
            ssize_t n = recv(fd, buf.data(), buf.size(), 0);
            if (n <= 0) {
                break;
            }
            total += static_cast<size_t>(n);
        }
        close(fd);
    });
    VM vm;
    const auto start = std::chrono::steady_clock::now();
    InterpretResult result = vm.interpret("var c = connect(\"127.0.0.1\", " +
                                          std::to_string(host.port()) +
                                          R"();
        var big = "x";
        var i = 0;
        while (i < 24) { big = big + big; i = i + 1; }
        c.write(big);
        c.close();
    )");
    peer.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(total.load(), size_t{1} << 24);
    EXPECT_LT(ms, 20000);
}

// ---------------------------------------------------------------------------
// The write side. A host peer reads late and slowly, so a write of a few
// megabytes must wait for room many times. Each test ends within a bound even
// when the program under test is wrong: the peer gives up after 5 seconds
// without data.
// ---------------------------------------------------------------------------

namespace {

// Waits for the first byte, then waits `delayMs` more, so the delay counts
// from the start of the program's write and not from the start of the test.
// Then reads until the end of the stream or 5 seconds without data, and hands
// each chunk to `onChunk`. Every `pauseEvery` bytes it sleeps 5 ms, so the
// writer meets a full connection again and again. True when the stream ended
// (the peer closed or reset), false when the 5 seconds passed first.
template <class F>
bool drainHost(int fd, int delayMs, size_t pauseEvery, F onChunk) {
    struct pollfd first = {fd, POLLIN, 0};
    poll(&first, 1, 10000);
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    std::string buf(65536, '\0');
    size_t sincePause = 0;
    for (;;) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 5000) <= 0) {
            return false;
        }
        ssize_t n = recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) {
            return true;
        }
        onChunk(buf.data(), static_cast<size_t>(n));
        sincePause += static_cast<size_t>(n);
        if (pauseEvery != 0 && sincePause >= pauseEvery) {
            sincePause = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

std::string connectToHost(const HostListener& host) {
    return "var c = connect(\"127.0.0.1\", " + std::to_string(host.port()) +
           ");\n";
}

long millisSince(std::chrono::steady_clock::time_point start) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

} // namespace

// A write that waits many times sends every byte once and in order: the
// offset of the parked call is the only record of the progress. The text is a
// counter pattern, so a byte sent twice or skipped shows at its position.
TEST_F(SocketParkTest, AWriteThatWaitsManyTimesSendsEveryByteInOrder) {
    HostListener host;
    std::atomic<size_t> total{0};
    std::atomic<size_t> wrong{0};
    std::thread peer([&] {
        int fd = host.accept();
        drainHost(fd, 100, 256 * 1024, [&](const char* p, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                if (p[i] != static_cast<char>('a' + (total + i) % 26)) {
                    ++wrong;
                }
            }
            total += n;
        });
        close(fd);
    });
    VM vm;
    const auto start = std::chrono::steady_clock::now();
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var alpha = "abcdefghijklmnopqrstuvwxyz";
        var big = alpha;
        var i = 0;
        while (i < 17) { big = big + big; i = i + 1; }
        var ticks = 0;
        var done = false;
        fun writer() { c.write(big); done = true; c.close(); }
        fun ticker() { while (!done) { ticks = ticks + 1; sleep(0.001); } }
        task.spawn(writer);
        task.spawn(ticker);
        while (!done) { sleep(0.005); }
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(total.load(), size_t{26} << 17);
    EXPECT_EQ(wrong.load(), 0u);
    EXPECT_LT(millisSince(start), 20000);
    EXPECT_NE(globalString(vm, "ticks"), "0");
}

// The newline of writeline is part of the offset: after many large lines that
// each wait, every line ends in exactly one newline at the right position.
TEST_F(SocketParkTest, ALargeWritelineKeepsItsNewlineAtTheEnd) {
    constexpr size_t kLine = size_t{1} << 17;
    constexpr size_t kLines = 120;
    HostListener host;
    std::atomic<size_t> total{0};
    std::atomic<size_t> wrong{0};
    std::thread peer([&] {
        int fd = host.accept();
        drainHost(fd, 100, 128 * 1024, [&](const char* p, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                const bool atEnd = (total + i) % (kLine + 1) == kLine;
                if ((p[i] == '\n') != atEnd) {
                    ++wrong;
                }
            }
            total += n;
        });
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var line = "x";
        var i = 0;
        while (i < 17) { line = line + line; i = i + 1; }
        var n = 0;
        while (n < 120) { c.writeline(line); n = n + 1; }
        c.close();
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(total.load(), (kLine + 1) * kLines);
    EXPECT_EQ(wrong.load(), 0u);
}

// Two tasks write 4 MiB each to one socket while the peer reads late. The
// bytes of one call stay together, and the call that began first sends first:
// the peer sees all of the first text and then all of the second.
TEST_F(SocketParkTest, TwoParkedWritersDoNotInterleaveAndKeepCallOrder) {
    constexpr size_t kHalf = size_t{1} << 22;
    HostListener host;
    std::string seen;
    seen.reserve(2 * kHalf);
    std::thread peer([&] {
        int fd = host.accept();
        drainHost(fd, 300, 0,
                  [&](const char* p, size_t n) { seen.append(p, n); });
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var a = "a"; var b = "b";
        var i = 0;
        while (i < 22) { a = a + a; b = b + b; i = i + 1; }
        var me = task.self();
        fun wa() { c.write(a); task.send(me, 1); }
        fun wb() { c.write(b); task.send(me, 1); }
        task.spawn(wa);
        task.spawn(wb);
        task.receive(); task.receive();
        c.close();
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    ASSERT_EQ(seen.size(), 2 * kHalf);
    EXPECT_EQ(seen.find_first_not_of('a'), kHalf);
    EXPECT_EQ(seen.find_first_not_of('b', kHalf), std::string::npos);
}

// The second call begins while the first still waits, and the connection has
// room at that moment because the peer reads in bursts. The second call must
// still wait for the first: the peer sees all of the first text, then all of
// the second.
TEST_F(SocketParkTest, ALateWriterWaitsBehindAParkedWriterEvenWhenThereIsRoom) {
    constexpr size_t kFirst = size_t{1} << 23;
    constexpr size_t kSecond = size_t{1} << 20;
    HostListener host;
    std::string seen;
    seen.reserve(kFirst + kSecond);
    std::thread peer([&] {
        int fd = host.accept();
        drainHost(fd, 100, 64 * 1024,
                  [&](const char* p, size_t n) { seen.append(p, n); });
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var a = "a"; var b = "b";
        var i = 0;
        while (i < 23) { a = a + a; i = i + 1; }
        i = 0;
        while (i < 20) { b = b + b; i = i + 1; }
        var me = task.self();
        fun wa() { c.write(a); task.send(me, 1); }
        fun wb() { sleep(0.2); c.write(b); task.send(me, 1); }
        task.spawn(wa);
        task.spawn(wb);
        task.receive(); task.receive();
        c.close();
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    ASSERT_EQ(seen.size(), kFirst + kSecond);
    EXPECT_EQ(seen.find_first_not_of('a'), kFirst);
    EXPECT_EQ(seen.find_first_not_of('b', kFirst), std::string::npos);
}

// A writer in a context where the task cannot suspend blocks its worker. It
// must not send its bytes into the middle of a parked write: it first sends
// the rest of the call that is ahead of it. After that, no byte of one text
// is split by the other.
TEST_F(SocketParkTest, ABlockingWriterSendsAfterTheParkedWriterAhead) {
    constexpr size_t kHalf = size_t{1} << 22;
    HostListener host;
    std::string seen;
    seen.reserve(2 * kHalf);
    std::thread peer([&] {
        int fd = host.accept();
        drainHost(fd, 300, 0,
                  [&](const char* p, size_t n) { seen.append(p, n); });
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var a = "a"; var b = "b";
        var i = 0;
        while (i < 22) { a = a + a; b = b + b; i = i + 1; }
        var me = task.self();
        var order = "";
        fun wa() {
            c.write(a); order = order + "A"; task.send(me, 1);
        }
        task.spawn(wa);
        var y = yield nil;
        class W { __str__() { c.write(b); order = order + "B"; return "w"; } }
        var s = str(W());
        task.receive();
        c.close();
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    ASSERT_EQ(seen.size(), 2 * kHalf);
    EXPECT_EQ(seen.find_first_not_of('a'), kHalf);
    EXPECT_EQ(seen.find_first_not_of('b', kHalf), std::string::npos);
    // The blocking writer sent the text ahead of it first. Its own call ended
    // first, and the task of the earlier call ran after it.
    EXPECT_EQ(globalString(vm, "order"), "BA");
}

// The write calls in every context where a task cannot suspend block the
// worker and do not raise: the ticker, a task of the same worker, runs after
// the write, and every byte arrives. The host reads late, so the connection is
// full and the write has to wait.
static void expectWriteBlocks(const std::string& context) {
    constexpr size_t kBytes = size_t{1} << 24;
    HostListener host;
    std::atomic<size_t> total{0};
    std::thread peer([&] {
        int fd = host.accept();
        drainHost(fd, 200, 0, [&](const char*, size_t n) { total += n; });
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var big = "x";
        var i = 0;
        while (i < 24) { big = big + big; i = i + 1; }
        var log = "";
        fun ticker() { log = log + "T"; }
        task.spawn(ticker);
    )" + context + R"(
        c.close();
        var y = yield nil;
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "WT");
    EXPECT_EQ(total.load(), kBytes);
}

TEST_F(SocketParkTest, AWriteInStrBlocksTheWorker) {
    expectWriteBlocks(R"(
        class A { __str__() { c.write(big); log = log + "W"; return "a"; } }
        var s = str(A());
    )");
}

TEST_F(SocketParkTest, AWritelineInStrBlocksTheWorker) {
    constexpr size_t kBytes = size_t{1} << 24;
    HostListener host;
    std::atomic<size_t> total{0};
    std::atomic<char> last{0};
    std::thread peer([&] {
        int fd = host.accept();
        drainHost(fd, 200, 0, [&](const char* p, size_t n) {
            total += n;
            last = p[n - 1];
        });
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var big = "x";
        var i = 0;
        while (i < 24) { big = big + big; i = i + 1; }
        var log = "";
        fun ticker() { log = log + "T"; }
        task.spawn(ticker);
        class A { __str__() { c.writeline(big); log = log + "W"; return "a"; } }
        var s = str(A());
        c.close();
        var y = yield nil;
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "WT");
    EXPECT_EQ(total.load(), kBytes + 1);
    EXPECT_EQ(last.load(), '\n');
}

TEST_F(SocketParkTest, AWriteInAMapKeyHashBlocksTheWorker) {
    expectWriteBlocks(R"(
        class K {
            __hash__() { c.write(big); log = log + "W"; return 1; }
            __eq__(o) { return true; }
        }
        var m = {};
        m[K()] = 1;
    )");
}

TEST_F(SocketParkTest, AWriteInCallMethodBlocksTheWorker) {
    expectWriteBlocks(R"(
        class M { run() { c.write(big); log = log + "W"; return 1; } }
        var r = callMethod(M(), "run");
    )");
}

TEST_F(SocketParkTest, AWriteInADeferredCallBlocksTheWorker) {
    expectWriteBlocks(R"(
        fun wr() { c.write(big); log = log + "W"; }
        fun f() { defer wr(); }
        f();
    )");
}

TEST_F(SocketParkTest, AWriteInANestedCoroutineBlocksTheWorker) {
    expectWriteBlocks(R"(
        fun inner() { c.write(big); log = log + "W"; return 1; }
        var co = coroutine.create(inner);
        class S { __str__() { co.resume(); return "s"; } }
        var s = str(S());
    )");
}

// A peer that never reads: the writer waits, the other tasks run, and a close
// by another task ends the call with SocketClosedError. The memory of the
// call does not grow with the wait.
TEST_F(SocketParkTest, ACloseEndsAWriteToAPeerThatNeverReads) {
    HostListener host;
    std::atomic<bool> release{false};
    std::thread peer([&] {
        int fd = host.accept();
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var big = "x";
        var i = 0;
        while (i < 26) { big = big + big; i = i + 1; }
        var me = task.self();
        var kind = "none"; var msg = "none";
        fun writer() {
            try { c.write(big); kind = "returned"; }
            catch (e) { kind = e.kind; msg = e.message; }
            task.send(me, 1);
        }
        task.spawn(writer);
        sleep(0.2);
        c.close();
        task.receive();
    )");
    release = true;
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "kind"), "SocketClosedError");
    EXPECT_EQ(globalString(vm, "msg"),
              "Cannot call 'write' on a closed socket.");
}

// A close fails parked writers in the order their calls began, with the name
// of each call.
TEST_F(SocketParkTest, ACloseFailsParkedWritersInCallOrder) {
    HostListener host;
    std::atomic<bool> release{false};
    std::thread peer([&] {
        int fd = host.accept();
        while (!release.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var big = "x";
        var i = 0;
        while (i < 26) { big = big + big; i = i + 1; }
        var me = task.self();
        var log = "";
        fun w1() { try { c.write(big); } catch (e) { log = log + "1" + e.message; } task.send(me, 1); }
        fun w2() { try { c.writeline(big); } catch (e) { log = log + "2" + e.message; } task.send(me, 1); }
        fun w3() { try { c.write("tail"); } catch (e) { log = log + "3" + e.message; } task.send(me, 1); }
        task.spawn(w1); task.spawn(w2); task.spawn(w3);
        sleep(0.2);
        c.close();
        task.receive(); task.receive(); task.receive();
    )");
    release = true;
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"),
              "1Cannot call 'write' on a closed socket."
              "2Cannot call 'writeline' on a closed socket."
              "3Cannot call 'write' on a closed socket.");
}

// close_write while a writer waits: the call fails with SocketClosedError,
// like a write after close_write(). The peer sees the bytes sent so far and
// then the end of the stream.
TEST_F(SocketParkTest, CloseWriteFailsAParkedWriter) {
    HostListener host;
    std::atomic<size_t> total{0};
    std::atomic<bool> sawEof{false};
    std::thread peer([&] {
        int fd = host.accept();
        sawEof =
            drainHost(fd, 400, 0, [&](const char*, size_t n) { total += n; });
        // The client keeps the socket open until it reads this verdict, so
        // the end of the stream can only come from close_write().
        const char* verdict = sawEof ? "eof\n" : "no-eof\n";
        send(fd, verdict, strlen(verdict), MSG_NOSIGNAL);
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var big = "x";
        var i = 0;
        while (i < 26) { big = big + big; i = i + 1; }
        var me = task.self();
        var kind = "none"; var msg = "none";
        fun writer() {
            try { c.write(big); kind = "returned"; }
            catch (e) { kind = e.kind; msg = e.message; }
            task.send(me, 1);
        }
        task.spawn(writer);
        sleep(0.1);
        c.close_write();
        task.receive();
        var again = "none";
        try { c.write("more"); } catch (e) { again = e.kind; }
        var verdict = c.readline();
        c.close();
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "verdict").rfind("eof", 0), 0u);
    EXPECT_EQ(globalString(vm, "kind"), "SocketClosedError");
    EXPECT_EQ(globalString(vm, "msg"),
              "Cannot write to a socket after close_write().");
    EXPECT_EQ(globalString(vm, "again"), "SocketClosedError");
    EXPECT_TRUE(sawEof.load());
    EXPECT_LT(total.load(), size_t{1} << 26);
}

// A peer that resets the connection while a writer waits: the call raises
// NetworkError (EPIPE or ECONNRESET), the other tasks ran meanwhile, and the
// writer queued behind it fails the same way.
TEST_F(SocketParkTest, APeerThatResetsFailsParkedWritersWithNetworkError) {
    HostListener host;
    std::thread peer([&] {
        int fd = host.accept();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        struct linger lg = {1, 0};
        setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var big = "x";
        var i = 0;
        while (i < 26) { big = big + big; i = i + 1; }
        var me = task.self();
        var k1 = "none"; var k2 = "none"; var ticks = 0;
        fun w1() { try { c.write(big); } catch (e) { k1 = e.kind; } task.send(me, 1); }
        fun w2() { try { c.writeline(big); } catch (e) { k2 = e.kind; } task.send(me, 1); }
        fun ticker() { ticks = ticks + 1; }
        task.spawn(w1); task.spawn(w2); task.spawn(ticker);
        task.receive(); task.receive();
        c.close();
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "k1"), "NetworkError");
    EXPECT_EQ(globalString(vm, "k2"), "NetworkError");
    EXPECT_EQ(globalString(vm, "ticks"), "1");
}

// EPIPE, with no task parked: a write to a peer that closed raises
// NetworkError, can be caught, and does not end the process by SIGPIPE.
TEST_F(SocketParkTest, EpipeIsANetworkErrorThatACatchSees) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        peer.close();
        var chunk = "x";
        var i = 0;
        while (i < 12) { chunk = chunk + chunk; i = i + 1; }
        var kind = "none";
        i = 0;
        while (i < 1000 and kind == "none") {
            try { conn.write(chunk); } catch (e) { kind = e.kind; }
            i = i + 1;
        }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "kind"), "NetworkError");
}

// A failed write does not change what a later read returns: the host sends
// "bye" and then closes while the program still writes, and the program reads
// "bye" after its write fails.
TEST_F(SocketParkTest, AFailedParkedWriteLeavesTheNextReadAlone) {
    HostListener host;
    std::thread peer([&] {
        int fd = host.accept();
        send(fd, "bye", 3, MSG_NOSIGNAL);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var big = "x";
        var i = 0;
        while (i < 26) { big = big + big; i = i + 1; }
        var kind = "none";
        try { c.write(big); } catch (e) { kind = e.kind; }
        var got = c.read_bytes(3);
        c.close();
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "kind"), "NetworkError");
    EXPECT_EQ(globalString(vm, "got"), "bye");
}

// Zero-length writes: write("") sends nothing, parks nothing and raises
// nothing, also when the connection is full; writeline("") sends one newline.
TEST_F(SocketParkTest, ZeroLengthWritesSendNothingAndWritelineSendsANewline) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        peer.write("");
        peer.writeline("");
        peer.write("ab");
        peer.close();
        var got = conn.read();
        var kind = "none";
        try { peer.write(""); } catch (e) { kind = e.kind; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "\nab");
    EXPECT_EQ(globalString(vm, "kind"), "SocketClosedError");
    EXPECT_FALSE(vm.runtime().hasPoller());
}

// A zero-length write returns at once, also when a writer is parked ahead of
// it on a full connection: it has no bytes to order.
TEST_F(SocketParkTest, AZeroLengthWriteDoesNotWaitBehindAParkedWriter) {
    HostListener host;
    std::thread peer([&] {
        int fd = host.accept();
        drainHost(fd, 300, 0, [](const char*, size_t) {});
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var big = "x";
        var i = 0;
        while (i < 26) { big = big + big; i = i + 1; }
        var me = task.self();
        var log = "";
        fun writer() { c.write(big); log = log + "W"; task.send(me, 1); }
        task.spawn(writer);
        var y = yield nil;
        c.write("");
        log = log + "Z";
        task.receive();
        c.close();
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "ZW");
}

// A write that is not full does not park and builds no poller.
TEST_F(SocketParkTest, AWriteThatFitsDoesNotPark) {
    VM vm;
    ASSERT_EQ(run(vm, R"(
        peer.write("hello"); peer.writeline(" world");
        var got = conn.readline();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "hello world");
    EXPECT_FALSE(vm.runtime().hasPoller());
}

// The end of the program closes the descriptor of a parked write: the host
// peer reads the end of the stream, and the program has not waited for the
// peer to read the whole text. The peer reads until the stream ends, so it
// sees the end only if the program closed the descriptor; the bytes it got
// are fewer than the text, so the write did not finish first. No wall-clock
// bound is used: building the text takes seconds in a debug build under load.
TEST_F(SocketParkTest, TheEndOfTheProgramClosesTheDescriptorOfAParkedWrite) {
    constexpr size_t kBytes = size_t{1} << 27;
    HostListener host;
    std::atomic<bool> sawEnd{false};
    std::atomic<size_t> total{0};
    std::thread peer([&] {
        int fd = host.accept();
        sawEnd =
            drainHost(fd, 1500, 0, [&](const char*, size_t n) { total += n; });
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        var big = "x";
        var i = 0;
        while (i < 27) { big = big + big; i = i + 1; }
        fun writer() { c.write(big); }
        task.spawn(writer);
        var y = yield nil;
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_TRUE(sawEnd.load())
        << "the peer waited for an end that did not come";
    EXPECT_LT(total.load(), kBytes);
}

// The collector runs while the call waits. The only holder of the text is the
// parked call: the writer builds it in the argument, and the ticker allocates
// meanwhile.
TEST_F(SocketParkTest, TheTextOfAParkedWriteStaysAliveAcrossCollections) {
    constexpr size_t kBytes = size_t{1} << 22;
    HostListener host;
    std::atomic<size_t> total{0};
    std::atomic<size_t> wrong{0};
    std::thread peer([&] {
        int fd = host.accept();
        drainHost(fd, 300, 0, [&](const char* p, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                if (p[i] != 'q') {
                    ++wrong;
                }
            }
            total += n;
        });
        close(fd);
    });
    VM vm;
    InterpretResult result = vm.interpret(connectToHost(host) + R"(
        fun make() { var s = "q"; var i = 0; while (i < 22) { s = s + s; i = i + 1; } return s; }
        var done = false; var junk = 0;
        fun writer() { c.write(make()); done = true; }
        fun ticker() {
            while (!done) {
                var j = 0;
                while (j < 200) { var t = "z" + str(j); junk = junk + len(t); j = j + 1; }
                sleep(0.002);
            }
        }
        task.spawn(writer);
        task.spawn(ticker);
        while (!done) { sleep(0.005); }
        c.close();
    )");
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(total.load(), kBytes);
    EXPECT_EQ(wrong.load(), 0u);
}

// Memory stays bounded: 3 writes of a 64 MiB text to a slow reader raise the
// resident set by far less than one more copy of the text, because the call
// keeps the text and an offset and copies nothing. The peak is sampled while
// the second program runs.
static size_t residentBytes() {
    FILE* f = fopen("/proc/self/statm", "r");
    if (f == nullptr) {
        return 0;
    }
    unsigned long size = 0, resident = 0;
    if (fscanf(f, "%lu %lu", &size, &resident) != 2) {
        resident = 0;
    }
    fclose(f);
    return static_cast<size_t>(resident) * 4096;
}

TEST_F(SocketParkTest, AWriteToASlowReaderDoesNotCopyTheText) {
    constexpr size_t kBytes = size_t{1} << 26;
    HostListener host;
    std::atomic<size_t> total{0};
    std::thread peer([&] {
        int fd = host.accept();
        drainHost(fd, 200, 0, [&](const char*, size_t n) { total += n; });
        close(fd);
    });
    VM vm;
    ASSERT_EQ(vm.interpret(connectToHost(host) + R"(
        var big = "x";
        var i = 0;
        while (i < 26) { big = big + big; i = i + 1; }
        var done = false;
    )"),
              InterpretResult::OK);
    const size_t base = residentBytes();
    std::atomic<size_t> peak{base};
    std::atomic<bool> stop{false};
    std::thread sampler([&] {
        while (!stop.load()) {
            size_t now = residentBytes();
            if (now > peak.load()) {
                peak = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    InterpretResult result = vm.interpret(R"(
        fun writer() { c.write(big); c.write(big); c.write(big); done = true; }
        task.spawn(writer);
        while (!done) { sleep(0.005); }
        c.close();
    )");
    stop = true;
    sampler.join();
    peer.join();
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(total.load(), 3 * kBytes);
    ASSERT_GT(base, size_t{0});
    EXPECT_LT(peak.load() - base, kBytes / 2);
}

// ---------------------------------------------------------------------------
// A write whose wait cannot be registered.
// ---------------------------------------------------------------------------

namespace {

// Runs in a forked child, because a defect here is a crash that would end the
// test runner. Returns the exit code: 0 when the program behaved.
//
// The write fills the send buffer and parks. The hook makes the registration
// of that one park fail, so the task is READY with the error and its entry is
// still in the writer queue of the socket. The second write calls the pump
// first. The pump must drop the stale entry, not step it: the park reset the
// call's state, so a step reads a null String.
int writeWithFailedRegistration() {
    alarm(60);
    HostListener host;
    std::atomic<bool> tailLast{false};
    std::thread peer([&] {
        int fd = host.accept();
        std::string last;
        drainHost(fd, 300, 0, [&](const char* p, size_t n) {
            last.append(p, n);
            if (last.size() > 4) {
                last.erase(0, last.size() - 4);
            }
        });
        tailLast = last == "tail";
        close(fd);
    });
    VM vm;
    int code = 0;
    if (vm.interpret(connectToHost(host)) != InterpretResult::OK) {
        code = 10;
    } else if (auto c = vm.getGlobal("c");
               !c.has_value() || asObjSocket(as<Obj*>(*c))->fd < 0) {
        code = 11;
    } else {
        const int fd = asObjSocket(as<Obj*>(*c))->fd;
        bool failed = false;
        EpollCtlFailHook hook = [&](int target) {
            if (target != fd || failed) {
                return 0;
            }
            failed = true;
            return ENOSPC;
        };
        setEpollCtlFailHook(&hook);
        const InterpretResult result = vm.interpret(R"(
            var big = "x";
            var i = 0;
            while (i < 24) { big = big + big; i = i + 1; }
            var kind = "none";
            var msg = "";
            try { c.write(big); kind = "wrote"; }
            catch (e) { kind = e.kind; msg = e.message; }
            c.write("tail");
            c.close();
        )");
        setEpollCtlFailHook(nullptr);
        auto kind = vm.getGlobal("kind");
        auto msg = vm.getGlobal("msg");
        if (result != InterpretResult::OK) {
            code = 12;
        } else if (!failed) {
            code = 13;
        } else if (!kind.has_value() || stringify(*kind) != "NetworkError") {
            code = 14;
        } else if (!msg.has_value() || stringify(*msg).find(std::strerror(
                                           ENOSPC)) == std::string::npos) {
            code = 15;
        }
    }
    peer.join();
    if (code == 0 && !tailLast.load()) {
        code = 16;
    }
    return code;
}

} // namespace

// A park that fails to register leaves its entry in the writer queue. The
// next write on the socket skips the entry, finishes, and the stream ends with
// the bytes of that write.
TEST_F(SocketParkTest, AFailedParkRegistrationLeavesNoLiveWriter) {
    fflush(nullptr);
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        _exit(writeWithFailedRegistration());
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status))
        << "the program ended on signal "
        << (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    EXPECT_EQ(WEXITSTATUS(status), 0) << "failed check " << WEXITSTATUS(status);
}
