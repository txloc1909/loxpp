// test_connect_park.cpp — connect() on a task that parks.
//
// No shared probe can show that connect suspends: a connection to the local
// host finishes at once. These tests give connect a peer that is slow to
// accept. The listener has a backlog of zero and its queue is full, so the
// kernel drops the SYN of the new connection, and the connection stays in
// progress until the host takes a connection from the queue. They check that
// the task parks and the worker goes on, that every context where a task
// cannot suspend blocks, that the address list is tried in order, and that the
// descriptor of an attempt is closed on each path. The `_gc` twin runs the
// suite under LOXPP_STRESS_GC.
//
// A native entry is not a context for connect: connect has two parameters, and
// an entry of a task receives none or one.

#include "container_objects.h"
#include "stdlib/net_api.h"
#include "test_harness.h"
#include "vm.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

std::string globalString(const VM& vm, const std::string& name) {
    auto v = vm.getGlobal(name);
    return v.has_value() ? stringify(*v) : "<undefined>";
}

int openDescriptorCount() {
    int count = 0;
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    while (readdir(dir) != nullptr) {
        ++count;
    }
    closedir(dir);
    return count;
}

// A listener on 127.0.0.1 whose accept queue is full. A connect to it stays
// in progress until `drain` runs. The fillers keep their connections open.
class SlowListener {
  public:
    SlowListener() {
        m_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        EXPECT_EQ(bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
                  0);
        EXPECT_EQ(listen(m_fd, 0), 0);
        socklen_t len = sizeof(addr);
        getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        fill(addr);
    }
    ~SlowListener() {
        stop();
        for (int fd : m_fillers) {
            close(fd);
        }
        if (m_fd >= 0) {
            close(m_fd);
        }
    }
    SlowListener(const SlowListener&) = delete;
    SlowListener& operator=(const SlowListener&) = delete;

    int port() const { return m_port; }
    bool full() const { return m_full; }

    // After `ms`, takes every connection from the queue until `stop`, so every
    // connection in progress gets in.
    void drainAfter(int ms) {
        m_drainer = std::thread([this, ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            const auto end =
                std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (!m_stop.load() && std::chrono::steady_clock::now() < end) {
                pollfd pfd = {m_fd, POLLIN, 0};
                if (poll(&pfd, 1, 50) > 0) {
                    int fd = ::accept(m_fd, nullptr, nullptr);
                    if (fd >= 0) {
                        m_taken.push_back(fd);
                    }
                }
            }
        });
    }
    // After `ms`, closes the listener: a connection still in progress gets a
    // reset at its next SYN.
    void closeAfter(int ms) {
        m_drainer = std::thread([this, ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            close(m_fd);
            m_fd = -1;
        });
    }
    void stop() {
        m_stop = true;
        if (m_drainer.joinable()) {
            m_drainer.join();
        }
        for (int fd : m_taken) {
            close(fd);
        }
        m_taken.clear();
    }

  private:
    // Opens connections until one stays in progress for 100 ms.
    void fill(const sockaddr_in& addr) {
        for (int i = 0; i < 8; ++i) {
            int fd =
                socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            int r = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr),
                              sizeof(addr));
            m_fillers.push_back(fd);
            if (r == 0) {
                continue;
            }
            pollfd pfd = {fd, POLLOUT, 0};
            if (poll(&pfd, 1, 100) == 0) {
                m_full = true;
                return;
            }
        }
    }

    int m_fd{-1};
    int m_port{0};
    bool m_full{false};
    std::vector<int> m_fillers;
    std::vector<int> m_taken;
    std::atomic<bool> m_stop{false};
    std::thread m_drainer;
};

} // namespace

#define REQUIRE_SLOW(listener)                                                 \
    if (!(listener).full()) {                                                  \
        GTEST_SKIP() << "the kernel did not hold the connection in progress";  \
    }

namespace {
int hostListen(int family, int& port);
} // namespace

class ConnectParkTest : public ::testing::Test {};

// The connect is in progress for about a second. The ticker, a task of the same
// worker, runs while the main task waits for the connection: "T" comes before
// "C". The poller exists, so the wait was a park and not a block.
TEST_F(ConnectParkTest, ASlowConnectSuspendsOnlyTheCallingTask) {
    SlowListener host;
    REQUIRE_SLOW(host);
    host.drainAfter(200);
    VM vm;
    ASSERT_EQ(vm.interpret("var log = \"\";"
                           "fun ticker() { log = log + \"T\"; }"
                           "task.spawn(ticker);"
                           "var c = connect(\"127.0.0.1\", " +
                           std::to_string(host.port()) +
                           R"();
        log = log + "C";
        var kind = type(c);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "TC");
    EXPECT_EQ(globalString(vm, "kind"), "Socket");
    EXPECT_TRUE(vm.runtime().hasPoller());
}

// The connected socket is nonblocking and does not survive an exec, whether the
// connect waited or finished at once.
static void expectSocketFlags(int port) {
    VM vm;
    ASSERT_EQ(vm.interpret("var c = connect(\"127.0.0.1\", " +
                           std::to_string(port) + ");"),
              InterpretResult::OK);
    auto v = vm.getGlobal("c");
    ASSERT_TRUE(v.has_value());
    const int fd = asObjSocket(as<Obj*>(*v))->fd;
    ASSERT_GE(fd, 0);
    EXPECT_NE(fcntl(fd, F_GETFD) & FD_CLOEXEC, 0);
    EXPECT_NE(fcntl(fd, F_GETFL) & O_NONBLOCK, 0);
}

TEST_F(ConnectParkTest, AParkedConnectGivesANonblockingCloseOnExecSocket) {
    SlowListener host;
    REQUIRE_SLOW(host);
    host.drainAfter(100);
    expectSocketFlags(host.port());
}

TEST_F(ConnectParkTest, AnImmediateConnectGivesANonblockingCloseOnExecSocket) {
    SlowListener probe;
    probe.stop();
    // The queue of this listener has room for one connection, so a connect
    // to a fresh listener finishes at once.
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ASSERT_EQ(listen(fd, 4), 0);
    socklen_t len = sizeof(addr);
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    expectSocketFlags(ntohs(addr.sin_port));
    close(fd);
}

// A connect that finishes before connect(2) returns, as it does on loopback,
// does not park: no poller exists afterwards, and the ticker runs after the
// call.
TEST_F(ConnectParkTest, AConnectThatFinishesAtOnceDoesNotPark) {
    int port = 0;
    int fd = hostListen(AF_INET, port);
    ASSERT_GE(fd, 0);
    VM vm;
    ASSERT_EQ(vm.interpret("var log = \"\";"
                           "fun ticker() { log = log + \"T\"; }"
                           "task.spawn(ticker);"
                           "var c = connect(\"127.0.0.1\", " +
                           std::to_string(port) +
                           R"();
        log = log + "C";
        var y = yield nil;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "CT");
    EXPECT_FALSE(vm.runtime().hasPoller());
    close(fd);
}

// Two tasks connect to the slow listener. Both park, both get a socket, and
// the ticker runs before either.
TEST_F(ConnectParkTest, ManyTasksConnectAtOnce) {
    SlowListener host;
    REQUIRE_SLOW(host);
    host.drainAfter(200);
    VM vm;
    ASSERT_EQ(vm.interpret("var log = \"\"; var me = task.self();"
                           "fun a() { var c = connect(\"127.0.0.1\", " +
                           std::to_string(host.port()) +
                           R"(); log = log + "A"; task.send(me, type(c)); }
        fun b() { var c = connect("127.0.0.1", )" +
                           std::to_string(host.port()) +
                           R"(); log = log + "B"; task.send(me, type(c)); }
        fun ticker() { log = log + "T"; }
        task.spawn(a); task.spawn(b); task.spawn(ticker);
        var x = task.receive();
        var y = task.receive();
        var sorted = log;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "x"), "Socket");
    EXPECT_EQ(globalString(vm, "y"), "Socket");
    EXPECT_EQ(globalString(vm, "log").front(), 'T');
    EXPECT_EQ(globalString(vm, "log").size(), 3u);
}

// The listener closes while the connect is in progress. The next SYN gets a
// reset; the wait ends, SO_ERROR holds the refusal, and the call raises a
// NetworkError that a catch receives. The descriptor of the attempt is closed.
TEST_F(ConnectParkTest, ARefusalAfterTheWaitIsANetworkError) {
    SlowListener host;
    REQUIRE_SLOW(host);
    host.closeAfter(200);
    const int before = openDescriptorCount();
    {
        VM vm;
        ASSERT_EQ(vm.interpret("var kind = \"none\"; var msg = \"\";"
                               "var log = \"\";"
                               "fun ticker() { log = log + \"T\"; }"
                               "task.spawn(ticker);"
                               "try { connect(\"127.0.0.1\", " +
                               std::to_string(host.port()) +
                               R"();
            kind = "connected"; } catch (e) { kind = e.kind; msg = e.message; }
        )"),
                  InterpretResult::OK);
        EXPECT_EQ(globalString(vm, "kind"), "NetworkError");
        EXPECT_NE(globalString(vm, "msg").find("Connection refused"),
                  std::string::npos)
            << globalString(vm, "msg");
        EXPECT_NE(globalString(vm, "msg").find("127.0.0.1:" +
                                               std::to_string(host.port())),
                  std::string::npos);
        EXPECT_EQ(globalString(vm, "log"), "T");
    }
    host.stop();
    // The fillers and the listener are the host's; the VM left none behind.
    EXPECT_EQ(openDescriptorCount(), before - 1) << "the listener closed";
}

// A refusal that arrives at once, with no wait, is the same error.
TEST_F(ConnectParkTest, AnImmediateRefusalIsANetworkError) {
    int port;
    {
        SlowListener probe;
        port = probe.port();
    }
    VM vm;
    ASSERT_EQ(vm.interpret("var kind = \"none\";"
                           "try { connect(\"127.0.0.1\", " +
                           std::to_string(port) +
                           R"();
            kind = "connected"; } catch (e) { kind = e.kind; }
        )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "kind"), "NetworkError");
}

TEST_F(ConnectParkTest, AFailedResolutionIsANetworkError) {
    VM vm;
    ASSERT_EQ(vm.interpret(R"(
        var kind = "none"; var msg = "";
        try { connect("no-such-host.invalid", 80); kind = "connected"; }
        catch (e) { kind = e.kind; msg = e.message; }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "kind"), "NetworkError");
    EXPECT_NE(globalString(vm, "msg").find("cannot resolve"), std::string::npos)
        << globalString(vm, "msg");
}

namespace {

// A listener on a loopback address of the given family; -1 when the host has
// no such address.
int hostListen(int family, int& port) {
    int fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    socklen_t len = 0;
    sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    if (family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&addr);
        a->sin_family = AF_INET;
        a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        len = sizeof(*a);
    } else {
        auto* a = reinterpret_cast<sockaddr_in6*>(&addr);
        a->sin6_family = AF_INET6;
        a->sin6_addr = in6addr_loopback;
        len = sizeof(*a);
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), len) != 0 ||
        listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(family == AF_INET
                     ? reinterpret_cast<sockaddr_in*>(&addr)->sin_port
                     : reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port);
    return fd;
}

// True when "localhost" resolves to both families.
bool localhostHasBothFamilies() {
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo("localhost", "80", &hints, &res) != 0) {
        return false;
    }
    bool v4 = false;
    bool v6 = false;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        v4 = v4 || p->ai_family == AF_INET;
        v6 = v6 || p->ai_family == AF_INET6;
    }
    freeaddrinfo(res);
    return v4 && v6;
}

// The listener has one family only, so the other address of "localhost" is
// refused: whichever comes first, one attempt fails and the next connects.
void expectFallback(int family) {
    if (!localhostHasBothFamilies()) {
        GTEST_SKIP() << "localhost has not both families";
    }
    int port = 0;
    int fd = hostListen(family, port);
    if (fd < 0) {
        GTEST_SKIP() << "no loopback address of this family";
    }
    const int before = openDescriptorCount();
    {
        VM vm;
        ASSERT_EQ(vm.interpret("var log = \"\";"
                               "fun ticker() { log = log + \"T\"; }"
                               "task.spawn(ticker);"
                               "var c = connect(\"localhost\", " +
                               std::to_string(port) +
                               R"();
            var kind = type(c);
            c.close();
        )"),
                  InterpretResult::OK);
        EXPECT_EQ(globalString(vm, "kind"), "Socket");
    }
    EXPECT_EQ(openDescriptorCount(), before) << "an attempt left a descriptor";
    close(fd);
}

} // namespace

TEST_F(ConnectParkTest, ARefusedAddressThenAGoodOneOnIpv4) {
    expectFallback(AF_INET);
}

TEST_F(ConnectParkTest, ARefusedAddressThenAGoodOneOnIpv6) {
    expectFallback(AF_INET6);
}

// A numeric IPv6 address needs no resolution and connects.
TEST_F(ConnectParkTest, ConnectToANumericIpv6Address) {
    int port = 0;
    int fd = hostListen(AF_INET6, port);
    if (fd < 0) {
        GTEST_SKIP() << "no IPv6 loopback";
    }
    VM vm;
    ASSERT_EQ(vm.interpret("var c = connect(\"::1\", " + std::to_string(port) +
                           R"();
        var kind = type(c);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "kind"), "Socket");
    close(fd);
}

// The addresses all fail: the error names the last errno, and no descriptor
// stays open.
TEST_F(ConnectParkTest, EveryAddressFailingLeavesNoDescriptor) {
    if (!localhostHasBothFamilies()) {
        GTEST_SKIP() << "localhost has not both families";
    }
    int port = 0;
    {
        int fd = hostListen(AF_INET, port);
        ASSERT_GE(fd, 0);
        close(fd);
    }
    const int before = openDescriptorCount();
    {
        VM vm;
        ASSERT_EQ(vm.interpret("var kind = \"none\";"
                               "try { connect(\"localhost\", " +
                               std::to_string(port) +
                               R"();
                kind = "connected"; } catch (e) { kind = e.kind; }
            )"),
                  InterpretResult::OK);
        EXPECT_EQ(globalString(vm, "kind"), "NetworkError");
    }
    EXPECT_EQ(openDescriptorCount(), before);
}

// The program ends while a task waits in connect. The descriptor of the
// attempt is closed with the task: no Socket holds it, so nothing else will.
TEST_F(ConnectParkTest, TheEndOfTheProgramClosesTheAttemptOfAParkedConnect) {
    SlowListener host;
    REQUIRE_SLOW(host);
    VM vm;
    // The first program makes the poller, so its descriptors are in the count.
    ASSERT_EQ(vm.interpret("sleep(0.001);"), InterpretResult::OK);
    const int before = openDescriptorCount();
    ASSERT_EQ(vm.interpret("fun waiter() { connect(\"127.0.0.1\", " +
                           std::to_string(host.port()) +
                           R"(); }
        task.spawn(waiter);
        var y = yield nil;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(openDescriptorCount(), before)
        << "the attempt stays open after the run";
}

// A collection runs while the connect waits: the ticker allocates hard. The
// call state holds no object, so nothing is lost, and the result is a Socket.
TEST_F(ConnectParkTest, ACollectionDuringTheWaitLosesNothing) {
    SlowListener host;
    REQUIRE_SLOW(host);
    host.drainAfter(300);
    VM vm;
    ASSERT_EQ(
        vm.interpret("var junk = 0;"
                     "fun ticker() {"
                     "  var i = 0;"
                     "  while (i < 20000) { var l = [i, str(i)]; i = i + 1; }"
                     "  junk = i;"
                     "}"
                     "task.spawn(ticker);"
                     "var c = connect(\"127.0.0.1\", " +
                     std::to_string(host.port()) +
                     R"();
        var kind = type(c);
    )"),
        InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "junk"), "20000");
    EXPECT_EQ(globalString(vm, "kind"), "Socket");
}

namespace {

// Where the task cannot suspend, connect blocks the worker and does not raise:
// the ticker, a task of the same worker, runs after the connect.
void expectConnectBlocks(const std::string& context) {
    SlowListener host;
    REQUIRE_SLOW(host);
    host.drainAfter(150);
    VM vm;
    InterpretResult result = vm.interpret("var log = \"\"; var got = \"none\";"
                                          "var port = " +
                                          std::to_string(host.port()) +
                                          R"(;
        fun ticker() { log = log + "T"; }
        task.spawn(ticker);
    )" + context +
                                          R"(
        var y = yield nil;
    )");
    ASSERT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "Socket");
    EXPECT_EQ(globalString(vm, "log"), "RT");
}

} // namespace

TEST_F(ConnectParkTest, AConnectInStrBlocksTheWorker) {
    expectConnectBlocks(R"(
        class A { __str__() {
            got = type(connect("127.0.0.1", port)); log = log + "R"; return "a";
        } }
        var s = str(A());
    )");
}

TEST_F(ConnectParkTest, AConnectInAMapKeyHashBlocksTheWorker) {
    expectConnectBlocks(R"(
        class K {
            __hash__() {
                got = type(connect("127.0.0.1", port)); log = log + "R";
                return 1;
            }
            __eq__(o) { return true; }
        }
        var m = {};
        m[K()] = 1;
    )");
}

TEST_F(ConnectParkTest, AConnectInCallMethodBlocksTheWorker) {
    expectConnectBlocks(R"(
        class M { run() {
            got = type(connect("127.0.0.1", port)); log = log + "R"; return 1;
        } }
        var r = callMethod(M(), "run");
    )");
}

TEST_F(ConnectParkTest, AConnectInADeferredCallBlocksTheWorker) {
    expectConnectBlocks(R"(
        fun cn() { got = type(connect("127.0.0.1", port)); log = log + "R"; }
        fun f() { defer cn(); }
        f();
    )");
}

TEST_F(ConnectParkTest, AConnectInANestedCoroutineBlocksTheWorker) {
    expectConnectBlocks(R"(
        fun inner() {
            got = type(connect("127.0.0.1", port)); log = log + "R"; return 1;
        }
        var co = coroutine.create(inner);
        class S { __str__() { co.resume(); return "s"; } }
        var s = str(S());
    )");
}

// A refusal that arrives while the worker blocks is a NetworkError that the
// caller catches, and does not hang.
TEST_F(ConnectParkTest, ABlockedConnectThatIsRefusedRaises) {
    SlowListener host;
    REQUIRE_SLOW(host);
    host.closeAfter(150);
    VM vm;
    ASSERT_EQ(vm.interpret("var kind = \"none\";"
                           "class A { __str__() {"
                           "  try { connect(\"127.0.0.1\", " +
                           std::to_string(host.port()) +
                           R"();
                } catch (e) { kind = e.kind; }
                return "a"; } }
            var s = str(A());
        )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "kind"), "NetworkError");
}

namespace {

// Sends SIGUSR1 to every thread of the process but its own, every 5 ms, with
// a handler that has no SA_RESTART, so poll(2) and epoll_wait(2) return EINTR.
class SignalStorm {
  public:
    SignalStorm() {
        struct sigaction act;
        memset(&act, 0, sizeof(act));
        act.sa_handler = [](int) {};
        sigemptyset(&act.sa_mask);
        memset(&m_old, 0, sizeof(m_old));
        sigaction(SIGUSR1, &act, &m_old);
        m_thread = std::thread([this] {
            const pid_t self = static_cast<pid_t>(syscall(SYS_gettid));
            while (!m_stop.load()) {
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
    }
    ~SignalStorm() {
        m_stop = true;
        m_thread.join();
        sigaction(SIGUSR1, &m_old, nullptr);
    }
    SignalStorm(const SignalStorm&) = delete;
    SignalStorm& operator=(const SignalStorm&) = delete;

  private:
    struct sigaction m_old;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

} // namespace

// Signals arrive during the whole connect. A call that blocks the worker
// waits for write readiness again after each signal and reads SO_ERROR once;
// it does not call connect(2) a second time.
TEST_F(ConnectParkTest, ABlockingConnectSurvivesSignals) {
    SlowListener host;
    REQUIRE_SLOW(host);
    host.drainAfter(150);
    SignalStorm storm;
    VM vm;
    ASSERT_EQ(vm.interpret("var got = \"none\";"
                           "class A { __str__() {"
                           "  got = type(connect(\"127.0.0.1\", " +
                           std::to_string(host.port()) +
                           R"()); return "a"; } }
            var s = str(A());
        )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "got"), "Socket");
}

// The same storm while the task is parked in the poller.
TEST_F(ConnectParkTest, AParkedConnectSurvivesSignals) {
    SlowListener host;
    REQUIRE_SLOW(host);
    host.drainAfter(150);
    SignalStorm storm;
    VM vm;
    ASSERT_EQ(vm.interpret("var log = \"\";"
                           "fun ticker() { log = log + \"T\"; }"
                           "task.spawn(ticker);"
                           "var c = connect(\"127.0.0.1\", " +
                           std::to_string(host.port()) +
                           R"();
        log = log + "C";
        var kind = type(c);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "TC");
    EXPECT_EQ(globalString(vm, "kind"), "Socket");
}

namespace {

// Applies hooks to connect() of this thread for the life of the object.
class HookGuard {
  public:
    explicit HookGuard(const ConnectTestHooks& hooks) : m_hooks(hooks) {
        setConnectTestHooks(&m_hooks);
    }
    ~HookGuard() { setConnectTestHooks(nullptr); }
    HookGuard(const HookGuard&) = delete;
    HookGuard& operator=(const HookGuard&) = delete;

  private:
    ConnectTestHooks m_hooks;
};

ConnectAddress loopbackAddress(int port) {
    ConnectAddress a;
    memset(&a, 0, sizeof(a));
    a.family = AF_INET;
    a.socktype = SOCK_STREAM;
    auto* in = reinterpret_cast<sockaddr_in*>(&a.addr);
    in->sin_family = AF_INET;
    in->sin_port = htons(static_cast<uint16_t>(port));
    in->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.len = sizeof(*in);
    return a;
}

// An address that connect(2) refuses at once with an error other than
// EINPROGRESS: an IPv4 socket and the address of another family. The result
// does not depend on the routes of the host.
ConnectAddress failingAddress() {
    ConnectAddress a = loopbackAddress(9);
    memset(&a.addr, 0, sizeof(a.addr));
    auto* in6 = reinterpret_cast<sockaddr_in6*>(&a.addr);
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(9);
    in6->sin6_addr = in6addr_loopback;
    a.len = sizeof(*in6);
    return a;
}

} // namespace

// The first address fails at once, with no wait. The next address is tried.
TEST_F(ConnectParkTest, AnImmediateErrorMovesToTheNextAddress) {
    {
        ConnectAddress bad = failingAddress();
        int fd = socket(bad.family, SOCK_STREAM, 0);
        ASSERT_GE(fd, 0);
        errno = 0;
        ASSERT_LT(
            ::connect(fd, reinterpret_cast<sockaddr*>(&bad.addr), bad.len), 0);
        ASSERT_NE(errno, EINPROGRESS)
            << "the host must refuse this address now";
        close(fd);
    }
    int port = 0;
    int fd = hostListen(AF_INET, port);
    ASSERT_GE(fd, 0);
    ConnectTestHooks hooks;
    hooks.addresses = [&](std::vector<ConnectAddress>& list) {
        list.clear();
        list.push_back(failingAddress());
        list.push_back(loopbackAddress(port));
    };
    HookGuard guard(hooks);
    const int before = openDescriptorCount();
    {
        VM vm;
        ASSERT_EQ(vm.interpret("var c = connect(\"127.0.0.1\", " +
                               std::to_string(port) +
                               R"();
            var kind = type(c);
            c.close();
        )"),
                  InterpretResult::OK);
        EXPECT_EQ(globalString(vm, "kind"), "Socket");
    }
    EXPECT_EQ(openDescriptorCount(), before);
    close(fd);
}

// The first address waits and then fails (the listener closes, and the next
// SYN gets a reset). The next address is tried and connects.
TEST_F(ConnectParkTest, AFailureAfterTheWaitMovesToTheNextAddress) {
    SlowListener slow;
    REQUIRE_SLOW(slow);
    slow.closeAfter(200);
    int goodPort = 0;
    int fd = hostListen(AF_INET, goodPort);
    ASSERT_GE(fd, 0);
    ConnectTestHooks hooks;
    const int slowPort = slow.port();
    hooks.addresses = [&](std::vector<ConnectAddress>& list) {
        list.clear();
        list.push_back(loopbackAddress(slowPort));
        list.push_back(loopbackAddress(goodPort));
    };
    HookGuard guard(hooks);
    VM vm;
    ASSERT_EQ(vm.interpret("var log = \"\";"
                           "fun ticker() { log = log + \"T\"; }"
                           "task.spawn(ticker);"
                           "var c = connect(\"127.0.0.1\", " +
                           std::to_string(slowPort) +
                           R"();
        log = log + "C";
        var kind = type(c);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "log"), "TC");
    EXPECT_EQ(globalString(vm, "kind"), "Socket");
    close(fd);
}

// connect(2) returns EINTR and the kernel goes on with the connection. The
// call waits for it like a connect in progress and does not call connect(2) a
// second time.
TEST_F(ConnectParkTest, AnInterruptedConnectIsAConnectInProgress) {
    int port = 0;
    int fd = hostListen(AF_INET, port);
    ASSERT_GE(fd, 0);
    int calls = 0;
    ConnectTestHooks hooks;
    hooks.connectCall = [&](int s, const sockaddr* sa, socklen_t len) {
        ++calls;
        int r = ::connect(s, sa, len);
        if (r < 0 && errno == EINPROGRESS) {
            errno = EINTR;
        }
        return r;
    };
    HookGuard guard(hooks);
    VM vm;
    ASSERT_EQ(vm.interpret("var c = connect(\"127.0.0.1\", " +
                           std::to_string(port) + R"();
        var kind = type(c);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(globalString(vm, "kind"), "Socket");
    EXPECT_EQ(calls, 1);
    close(fd);
}

// A poll(2) that fails says nothing about the connect: the attempt fails, and
// the call does not report a success. EINTR is tried again.
TEST_F(ConnectParkTest, AFailingPollIsAFaultAndAnInterruptedPollIsRetried) {
    int port = 0;
    int fd = hostListen(AF_INET, port);
    ASSERT_GE(fd, 0);
    for (int failure : {ENOMEM, EINTR}) {
        int polls = 0;
        ConnectTestHooks hooks;
        hooks.pollCall = [&](int s) {
            if (polls++ == 0) {
                errno = failure;
                return -1;
            }
            pollfd pfd = {s, POLLOUT, 0};
            return ::poll(&pfd, 1, 0);
        };
        HookGuard guard(hooks);
        VM vm;
        ASSERT_EQ(vm.interpret("var kind = \"none\";"
                               "try { var c = connect(\"127.0.0.1\", " +
                               std::to_string(port) +
                               R"();
                kind = type(c); } catch (e) { kind = e.kind; }
            )"),
                  InterpretResult::OK);
        EXPECT_EQ(globalString(vm, "kind"),
                  failure == ENOMEM ? "NetworkError" : "Socket");
    }
    close(fd);
}

// A poll(2) failure ends the attempt, and the descriptor of the attempt is
// closed: at once when connect(2) returns, and after a park when the wait
// ends. The count of open descriptors is the same before and after.
TEST_F(ConnectParkTest, AFailingPollClosesTheDescriptorOfTheAttempt) {
    int port = 0;
    int fd = hostListen(AF_INET, port);
    ASSERT_GE(fd, 0);
    const int before = openDescriptorCount();
    {
        ConnectTestHooks hooks;
        hooks.pollCall = [](int) {
            errno = ENOMEM;
            return -1;
        };
        HookGuard guard(hooks);
        VM vm;
        ASSERT_EQ(vm.interpret("var kind = \"none\";"
                               "try { connect(\"127.0.0.1\", " +
                               std::to_string(port) +
                               R"();
                kind = "connected"; } catch (e) { kind = e.kind; }
            )"),
                  InterpretResult::OK);
        EXPECT_EQ(globalString(vm, "kind"), "NetworkError");
    }
    EXPECT_EQ(openDescriptorCount(), before);
    close(fd);
}

TEST_F(ConnectParkTest, AFailingPollAfterTheWaitClosesTheDescriptor) {
    SlowListener host;
    REQUIRE_SLOW(host);
    host.drainAfter(200);
    const int before = openDescriptorCount();
    {
        int polls = 0;
        ConnectTestHooks hooks;
        // The first poll finds the connect in progress and the task parks.
        // The poll after the wake fails.
        hooks.pollCall = [&](int s) {
            if (polls++ == 0) {
                pollfd pfd = {s, POLLOUT, 0};
                return ::poll(&pfd, 1, 0);
            }
            errno = ENOMEM;
            return -1;
        };
        HookGuard guard(hooks);
        VM vm;
        ASSERT_EQ(vm.interpret("var kind = \"none\";"
                               "try { connect(\"127.0.0.1\", " +
                               std::to_string(host.port()) +
                               R"();
                kind = "connected"; } catch (e) { kind = e.kind; }
            )"),
                  InterpretResult::OK);
        EXPECT_EQ(globalString(vm, "kind"), "NetworkError");
        EXPECT_GE(polls, 2);
    }
    // The drainer closes the connections it took, so only the VM can leave a
    // descriptor behind.
    host.stop();
    EXPECT_EQ(openDescriptorCount(), before);
}
