// test_poller.cpp — the wait set of one worker (src/poller.h) and the idle
// accounting that knows about it (WorkerGroup::goIdle, src/worker.h).

#include "poller.h"
#include "worker.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <set>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

long msSince(Clock::time_point start) {
    return std::chrono::duration_cast<milliseconds>(Clock::now() - start)
        .count();
}

struct Pipe {
    int rd{-1};
    int wr{-1};
    Pipe() {
        int fds[2];
        EXPECT_EQ(::pipe2(fds, O_CLOEXEC | O_NONBLOCK), 0);
        rd = fds[0];
        wr = fds[1];
    }
    ~Pipe() {
        closeRead();
        if (wr >= 0) {
            ::close(wr);
        }
    }
    void closeRead() {
        if (rd >= 0) {
            ::close(rd);
            rd = -1;
        }
    }
    void put() const {
        char c = 'x';
        ASSERT_EQ(::write(wr, &c, 1), 1);
    }
};

std::set<int> openFds() {
    std::set<int> out;
    DIR* dir = ::opendir("/proc/self/fd");
    while (dirent* e = ::readdir(dir)) {
        const int fd = std::atoi(e->d_name);
        if (e->d_name[0] != '.' && fd != ::dirfd(dir)) {
            out.insert(fd);
        }
    }
    ::closedir(dir);
    return out;
}

} // namespace

// A wake from another thread ends a wait that has no wait registered.
TEST(PollerTest, WakeFromAnotherThreadEndsTheWait) {
    Poller poller;
    std::thread waker([&] {
        std::this_thread::sleep_for(milliseconds(50));
        poller.wake();
    });
    std::vector<Poller::Token> ready{99};
    const auto start = Clock::now();
    poller.wait(ready);
    waker.join();
    EXPECT_TRUE(ready.empty());
    EXPECT_GE(msSince(start), 40);
    EXPECT_LT(msSince(start), 2000);
}

// A wake that comes before the wait is kept: the check-then-sleep gap of the
// worker loop cannot lose it.
TEST(PollerTest, WakeBeforeTheWaitIsKept) {
    Poller poller;
    poller.wake();
    std::vector<Poller::Token> ready;
    const auto start = Clock::now();
    poller.wait(ready);
    EXPECT_TRUE(ready.empty());
    EXPECT_LT(msSince(start), 1000);
    // The wake was used up: a timer decides the next wait.
    const auto token = poller.waitUntil(Clock::now() + milliseconds(60));
    poller.wait(ready);
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready[0], token);
}

TEST(PollerTest, ReadyDescriptorIsReportedOnceAndRemoved) {
    Poller poller;
    Pipe p;
    const auto token = poller.waitFd(p.rd, Poller::Interest::Read);
    ASSERT_NE(token, 0U);
    EXPECT_TRUE(poller.hasWaits());
    std::thread writer([&] {
        std::this_thread::sleep_for(milliseconds(50));
        p.put();
    });
    std::vector<Poller::Token> ready;
    poller.wait(ready);
    writer.join();
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready[0], token);
    EXPECT_FALSE(poller.hasWaits());
    EXPECT_FALSE(poller.cancel(token));
}

TEST(PollerTest,
     ReportsDescriptorsInRegistrationOrderThenTimersInDeadlineOrder) {
    Poller poller;
    Pipe a;
    Pipe b;
    const auto late = poller.waitUntil(Clock::now() + milliseconds(40));
    const auto early = poller.waitUntil(Clock::now() + milliseconds(20));
    // b registers first and becomes ready last.
    const auto tb = poller.waitFd(b.rd, Poller::Interest::Read);
    const auto ta = poller.waitFd(a.rd, Poller::Interest::Read);
    a.put();
    b.put();
    std::this_thread::sleep_for(milliseconds(80));
    std::vector<Poller::Token> ready;
    poller.wait(ready);
    EXPECT_EQ(ready, (std::vector<Poller::Token>{tb, ta, early, late}));
}

// A call that waits again takes the token of its first wait, so it keeps its
// place among the waits that began after it.
TEST(PollerTest, AWaitThatTakesAnOldTokenKeepsItsPlace) {
    Poller poller;
    Pipe a;
    Pipe b;
    const auto ta = poller.waitFd(a.rd, Poller::Interest::Read);
    const auto tb = poller.waitFd(b.rd, Poller::Interest::Read);
    a.put();
    std::vector<Poller::Token> ready;
    poller.wait(ready);
    ASSERT_EQ(ready, (std::vector<Poller::Token>{ta}));
    ASSERT_EQ(poller.waitFd(a.rd, Poller::Interest::Read, ta), ta);
    b.put();
    poller.wait(ready);
    EXPECT_EQ(ready, (std::vector<Poller::Token>{ta, tb}));
}

TEST(PollerTest, AWaitThatTakesAnOldTokenKeepsItsPlaceOnOneDescriptor) {
    Poller poller;
    Pipe p;
    const auto first = poller.waitFd(p.wr, Poller::Interest::Write);
    const auto second = poller.waitFd(p.wr, Poller::Interest::Write);
    ASSERT_TRUE(poller.cancel(first));
    ASSERT_EQ(poller.waitFd(p.wr, Poller::Interest::Write, first), first);
    std::vector<Poller::Token> cancelled;
    poller.cancelFd(p.wr, cancelled);
    EXPECT_EQ(cancelled, (std::vector<Poller::Token>{first, second}));
}

TEST(PollerTest, WriteInterestAndReadInterestOnOneDescriptor) {
    Poller poller;
    Pipe p;
    // The write end of an empty pipe is writable; the read end is not
    // readable. Two waits on the read end of a socketpair would do the
    // same for both; this checks that the mask follows the records.
    const auto w = poller.waitFd(p.wr, Poller::Interest::Write);
    const auto r = poller.waitFd(p.wr, Poller::Interest::Read);
    std::vector<Poller::Token> ready;
    poller.wait(ready);
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready[0], w);
    EXPECT_TRUE(poller.cancel(r));
    EXPECT_FALSE(poller.hasWaits());
}

TEST(PollerTest, CancelledTimerIsNotReported) {
    Poller poller;
    const auto gone = poller.waitUntil(Clock::now() + milliseconds(10));
    const auto kept = poller.waitUntil(Clock::now() + milliseconds(30));
    EXPECT_TRUE(poller.cancel(gone));
    std::vector<Poller::Token> ready;
    poller.wait(ready);
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready[0], kept);
}

// A wake in the middle of a timed wait ends that wait early. The next wait
// uses the time that is left, not a fresh full timeout.
TEST(PollerTest, TimeoutIsComputedAgainAfterAWake) {
    Poller poller;
    const auto start = Clock::now();
    const auto token = poller.waitUntil(start + milliseconds(300));
    std::thread waker([&] {
        std::this_thread::sleep_for(milliseconds(100));
        poller.wake();
    });
    std::vector<Poller::Token> ready;
    poller.wait(ready);
    waker.join();
    EXPECT_TRUE(ready.empty());
    EXPECT_LT(msSince(start), 250);
    poller.wait(ready);
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready[0], token);
    EXPECT_GE(msSince(start), 295);
    EXPECT_LT(msSince(start), 1500);
}

namespace {
void onSignal(int) {}
} // namespace

// A signal that interrupts epoll_wait does not end the wait or move the
// deadline.
TEST(PollerTest, InterruptedWaitContinuesToTheDeadline) {
    // memset, not `{}`: clang-format versions differ on the spacing of an
    // empty braced init for a multi-word struct type.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: epoll_wait fails with EINTR
    struct sigaction old;
    std::memset(&old, 0, sizeof(old));
    ASSERT_EQ(::sigaction(SIGUSR2, &sa, &old), 0);
    Poller poller;
    const auto start = Clock::now();
    const auto token = poller.waitUntil(start + milliseconds(250));
    const pthread_t self = pthread_self();
    std::thread signaller([&] {
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(milliseconds(30));
            pthread_kill(self, SIGUSR2);
        }
    });
    std::vector<Poller::Token> ready;
    poller.wait(ready);
    signaller.join();
    ::sigaction(SIGUSR2, &old, nullptr);
    ASSERT_EQ(ready.size(), 1U);
    EXPECT_EQ(ready[0], token);
    EXPECT_GE(msSince(start), 245);
    EXPECT_LT(msSince(start), 1500);
}

// A closed descriptor whose number a new descriptor gets must not carry the
// old record: epoll keeps no entry for it once cancelFd ran.
TEST(PollerTest, CancelledDescriptorLeavesNoRecordForItsNumber) {
    Poller poller;
    auto first = std::make_unique<Pipe>();
    const int number = first->rd;
    const auto stale = poller.waitFd(number, Poller::Interest::Read);
    std::vector<Poller::Token> cancelled;
    poller.cancelFd(number, cancelled);
    EXPECT_EQ(cancelled, (std::vector<Poller::Token>{stale}));
    EXPECT_FALSE(poller.hasWaits());
    first->closeRead();
    Pipe second;
    ASSERT_EQ(second.rd, number) << "the number was not reused";
    second.put();
    const auto timer = poller.waitUntil(Clock::now() + milliseconds(40));
    std::vector<Poller::Token> ready;
    poller.wait(ready);
    // Only the timer: the new descriptor is not registered.
    EXPECT_EQ(ready, (std::vector<Poller::Token>{timer}));
    // And it can be registered again.
    EXPECT_NE(poller.waitFd(number, Poller::Interest::Read), 0U);
}

namespace {
long threadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
} // namespace

// The epoll entry belongs to the open file description, not to the number. A
// copy of the descriptor keeps the entry alive after close(), and a level-
// triggered entry that nobody reads would end every wait at once.
TEST(PollerTest, CancelledDescriptorDoesNotKeepWakingTheWait) {
    Poller poller;
    Pipe p;
    const int copy = ::dup(p.rd);
    ASSERT_GE(copy, 0);
    poller.waitFd(p.rd, Poller::Interest::Read);
    std::vector<Poller::Token> cancelled;
    poller.cancelFd(p.rd, cancelled);
    p.closeRead();
    p.put();
    const auto timer = poller.waitUntil(Clock::now() + milliseconds(150));
    const long cpu = threadCpuMs();
    std::vector<Poller::Token> ready;
    poller.wait(ready);
    EXPECT_LT(threadCpuMs() - cpu, 50);
    EXPECT_EQ(ready, (std::vector<Poller::Token>{timer}));
    ::close(copy);
}

TEST(PollerTest, RefusedDescriptorGivesNoToken) {
    Poller poller;
    const int file = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    ASSERT_GE(file, 0);
    EXPECT_EQ(poller.waitFd(file, Poller::Interest::Read), 0U);
    EXPECT_EQ(errno, EPERM);
    EXPECT_FALSE(poller.hasWaits());
    ::close(file);
}

// run() forks: the descriptors of a poller must not reach a child.
TEST(PollerTest, DescriptorsAreCloseOnExecAndClosedWithThePoller) {
    const std::set<int> before = openFds();
    {
        Poller poller;
        std::set<int> added;
        for (int fd : openFds()) {
            if (before.count(fd) == 0) {
                added.insert(fd);
            }
        }
        EXPECT_EQ(added.size(), 2U);
        for (int fd : added) {
            const int flags = ::fcntl(fd, F_GETFD);
            ASSERT_GE(flags, 0);
            EXPECT_NE(flags & FD_CLOEXEC, 0) << "fd " << fd;
        }
    }
    EXPECT_EQ(openFds(), before);
}

// --- Idle accounting (C7) ----------------------------------------------------

namespace {

// Two workers of one group, built by hand: the main inbox and one more.
struct TwoWorkers {
    std::shared_ptr<WorkerGroup> group = std::make_shared<WorkerGroup>();
    Inbox mainInbox;
    Inbox other;
    TwoWorkers() {
        group->mainInbox = &mainInbox;
        group->addWorker(&mainInbox);
        group->addWorker(&other);
        group->busy.store(2);
        mainInbox.group = group;
        other.group = group;
    }
};

} // namespace

TEST(IdleAccountingTest, WorkerWithARegisteredWaitStaysBusy) {
    TwoWorkers w;
    EXPECT_FALSE(w.group->goIdle(w.other, WaitReason::Poller));
    EXPECT_FALSE(w.other.idle);
    EXPECT_EQ(w.group->busy.load(), 2);
    // The main worker waits in receive with nothing inbound. The other can
    // still end a wait, so this is not a deadlock.
    EXPECT_FALSE(w.group->goIdle(w.mainInbox));
    EXPECT_FALSE(w.group->deadlock.load());
    EXPECT_EQ(w.group->busy.load(), 1);
}

TEST(IdleAccountingTest, RealDeadlockIsStillReported) {
    TwoWorkers w;
    EXPECT_FALSE(w.group->goIdle(w.other, WaitReason::Inbox));
    EXPECT_TRUE(w.group->goIdle(w.mainInbox, WaitReason::Inbox));
    EXPECT_TRUE(w.group->deadlock.load());
}

// The wait ended and the worker waits for a message only: now it counts.
TEST(IdleAccountingTest, WorkerThatLosesItsWaitsCountsAsIdle) {
    TwoWorkers w;
    EXPECT_FALSE(w.group->goIdle(w.mainInbox));
    EXPECT_FALSE(w.group->goIdle(w.other, WaitReason::Poller));
    EXPECT_FALSE(w.group->deadlock.load());
    EXPECT_TRUE(w.group->goIdle(w.other, WaitReason::Inbox));
    EXPECT_TRUE(w.group->deadlock.load() || w.group->busy.load() == 0);
}

// The end of the program wakes a worker that sleeps in its poller.
TEST(IdleAccountingTest, EndOfTheProgramWakesAWorkerThatSleepsInItsPoller) {
    TwoWorkers w;
    Poller poller;
    Pipe unused;
    poller.waitFd(unused.rd, Poller::Interest::Read);
    // A bound, so that a lost wake fails the test and does not hang it.
    poller.waitUntil(Clock::now() + std::chrono::seconds(3));
    {
        std::lock_guard<std::mutex> lock(w.other.mutex);
        w.other.poller = &poller;
        w.other.polling = true;
    }
    std::thread ender([&] {
        std::this_thread::sleep_for(milliseconds(50));
        w.group->end();
    });
    std::vector<Poller::Token> ready;
    const auto start = Clock::now();
    poller.wait(ready);
    ender.join();
    EXPECT_TRUE(ready.empty());
    EXPECT_LT(msSince(start), 2000);
    EXPECT_TRUE(w.group->ended.load());
    {
        std::lock_guard<std::mutex> lock(w.other.mutex);
        w.other.polling = false;
        w.other.poller = nullptr;
    }
}

// A send to a worker that sleeps in its poller writes the wakeup descriptor.
TEST(IdleAccountingTest, SendWakesAWorkerThatSleepsInItsPoller) {
    TwoWorkers w;
    Poller poller;
    Pipe unused;
    poller.waitFd(unused.rd, Poller::Interest::Read);
    // A bound, so that a lost wake fails the test and does not hang it.
    poller.waitUntil(Clock::now() + std::chrono::seconds(3));
    {
        std::lock_guard<std::mutex> lock(w.other.mutex);
        w.other.poller = &poller;
        w.other.polling = true;
    }
    std::thread sender([&] {
        std::this_thread::sleep_for(milliseconds(50));
        EXPECT_TRUE(postEnvelope(w.other, Envelope{nullptr, Value{}}));
    });
    std::vector<Poller::Token> ready;
    const auto start = Clock::now();
    poller.wait(ready);
    sender.join();
    EXPECT_TRUE(ready.empty());
    EXPECT_LT(msSince(start), 2000);
    EXPECT_TRUE(w.other.nonEmpty.load());
    {
        std::lock_guard<std::mutex> lock(w.other.mutex);
        w.other.polling = false;
        w.other.poller = nullptr;
    }
}
