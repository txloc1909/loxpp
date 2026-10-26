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
