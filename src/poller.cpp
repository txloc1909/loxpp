#include "poller.h"

#include "worker.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace {

constexpr int kMaxEvents = 64;

[[noreturn]] void fatal(const char* what) {
    std::fprintf(stderr, "Cannot set up the wait set of a worker: %s failed.\n",
                 what);
    endProcess(70);
}

std::uint32_t eventBit(Poller::Interest interest) {
    return interest == Poller::Interest::Read ? EPOLLIN : EPOLLOUT;
}

} // namespace

Poller::Poller() {
    // CLOEXEC: run() forks, and a child must not keep these open.
    m_epoll = ::epoll_create1(EPOLL_CLOEXEC);
    if (m_epoll < 0) {
        fatal("epoll_create1");
    }
    m_wakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_wakeFd < 0) {
        fatal("eventfd");
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = m_wakeFd;
    if (::epoll_ctl(m_epoll, EPOLL_CTL_ADD, m_wakeFd, &ev) != 0) {
        fatal("epoll_ctl");
    }
}

Poller::~Poller() {
    // The descriptors that waits name belong to their callers: not closed here.
    ::close(m_wakeFd);
    ::close(m_epoll);
}

static thread_local const EpollCtlFailHook* s_epollCtlFailHook = nullptr;

void setEpollCtlFailHook(const EpollCtlFailHook* hook) {
    s_epollCtlFailHook = hook;
}

std::uint32_t Poller::FdEntry::wanted() const {
    return (read != 0 ? eventBit(Interest::Read) : 0U) |
           (write != 0 ? eventBit(Interest::Write) : 0U);
}

Poller::Token Poller::waitFd(int fd, Interest interest, Token again) {
    FdEntry& entry = m_fds[fd];
    const bool fresh = entry.read == 0 && entry.write == 0;
    if (entry.slot(interest) != 0) {
        errno = EBUSY;
        return 0;
    }
    const std::uint32_t oldMask = entry.mask;
    const std::uint32_t mask = oldMask | eventBit(interest);
    if (fresh || mask != oldMask) {
        epoll_event ev{};
        ev.events = mask;
        ev.data.fd = fd;
        const int op = fresh ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
        // Only a call that changes the watch set reaches this check, so the
        // load sits beside a system call and costs nothing measurable.
        const int injected =
            s_epollCtlFailHook != nullptr ? (*s_epollCtlFailHook)(fd) : 0;
        if (injected != 0 || ::epoll_ctl(m_epoll, op, fd, &ev) != 0) {
            const int saved = injected != 0 ? injected : errno;
            if (fresh) {
                m_fds.erase(fd);
            }
            errno = saved;
            return 0;
        }
    }
    entry.mask = mask;
    const Token token = again != 0 ? again : m_nextToken++;
    entry.slot(interest) = token;
    m_tokenFd.emplace(token, fd);
    return token;
}

Poller::Token Poller::waitUntil(Clock::time_point deadline) {
    const Token token = m_nextToken++;
    m_timerQueue.emplace(deadline, token);
    m_timers.emplace(token, deadline);
    return token;
}

Poller::Clock::time_point Poller::deadlineAfter(double seconds) {
    // The 1 second margin keeps the double conversion below the clock's range.
    const auto now = Clock::now();
    const double room =
        std::chrono::duration<double>(Clock::time_point::max() - now).count();
    if (seconds >= room - 1.0) {
        return Clock::time_point::max();
    }
    return now + std::chrono::duration_cast<Clock::duration>(
                     std::chrono::duration<double>(seconds));
}

void Poller::dropFdWait(int fd, Token token) {
    auto found = m_fds.find(fd);
    if (found == m_fds.end()) {
        return;
    }
    FdEntry& entry = found->second;
    if (entry.read == token) {
        entry.read = 0;
    }
    if (entry.write == token) {
        entry.write = 0;
    }
    m_tokenFd.erase(token);
    if (entry.read == 0 && entry.write == 0) {
        // EBADF and ENOENT: the caller closed the descriptor first. Nothing
        // is left to remove, so the failure is the same as the success.
        ::epoll_ctl(m_epoll, EPOLL_CTL_DEL, fd, nullptr);
        m_fds.erase(found);
        return;
    }
    const std::uint32_t mask = entry.wanted();
    if (mask != entry.mask) {
        epoll_event ev{};
        ev.events = mask;
        ev.data.fd = fd;
        ::epoll_ctl(m_epoll, EPOLL_CTL_MOD, fd, &ev);
        entry.mask = mask;
    }
}

bool Poller::cancel(Token token) {
    if (auto timer = m_timers.find(token); timer != m_timers.end()) {
        m_timerQueue.erase({timer->second, token});
        m_timers.erase(timer);
        return true;
    }
    if (auto fd = m_tokenFd.find(token); fd != m_tokenFd.end()) {
        dropFdWait(fd->second, token);
        return true;
    }
    return false;
}

void Poller::cancelFd(int fd, std::vector<Token>& cancelled) {
    auto found = m_fds.find(fd);
    if (found == m_fds.end()) {
        return;
    }
    const FdEntry& entry = found->second;
    Token first = entry.read;
    Token second = entry.write;
    if (second != 0 && (first == 0 || second < first)) {
        std::swap(first, second);
    }
    for (Token token : {first, second}) {
        if (token != 0) {
            cancelled.push_back(token);
            m_tokenFd.erase(token);
        }
    }
    ::epoll_ctl(m_epoll, EPOLL_CTL_DEL, fd, nullptr);
    m_fds.erase(found);
}

void Poller::wake() const {
    const std::uint64_t one = 1;
    // A full counter (EAGAIN) means a wake is already pending. EINTR cannot
    // happen on a nonblocking eventfd write.
    ssize_t n = ::write(m_wakeFd, &one, sizeof one);
    (void)n;
}

int Poller::nextTimeoutMs() const {
    if (m_timerQueue.empty()) {
        return -1;
    }
    const auto left = m_timerQueue.begin()->first - Clock::now();
    if (left <= Clock::duration::zero()) {
        return 0;
    }
    // Round up: a wait that ends a little early would spin.
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
    return static_cast<int>(std::min<long long>(ms, INT_MAX));
}

void Poller::collectFdWaits(const epoll_event& ev,
                            std::vector<Token>& fired) const {
    auto found = m_fds.find(ev.data.fd);
    if (found == m_fds.end()) {
        return;
    }
    // An error or a hangup is ready for every interest: the call that waits
    // reads or writes and sees the error itself.
    const bool failed = (ev.events & (EPOLLERR | EPOLLHUP)) != 0;
    const FdEntry& entry = found->second;
    for (Interest interest : {Interest::Read, Interest::Write}) {
        const Token token =
            interest == Interest::Read ? entry.read : entry.write;
        if (token != 0 && (failed || (ev.events & eventBit(interest)) != 0)) {
            fired.push_back(token);
        }
    }
}

void Poller::collectTimers(std::vector<Token>& ready) {
    const auto now = Clock::now();
    while (!m_timerQueue.empty() && m_timerQueue.begin()->first <= now) {
        const Token token = m_timerQueue.begin()->second;
        m_timerQueue.erase(m_timerQueue.begin());
        m_timers.erase(token);
        ready.push_back(token);
    }
}

void Poller::wait(std::vector<Token>& ready, bool block) {
    ready.clear();
    std::array<epoll_event, kMaxEvents> events{};
    for (;;) {
        // Computed again on every pass, so an early return or a signal does
        // not stretch or cut the wait.
        const int n = ::epoll_wait(m_epoll, events.data(), kMaxEvents,
                                   block ? nextTimeoutMs() : 0);
        if (n < 0 && errno != EINTR) {
            fatal("epoll_wait");
        }
        bool woken = false;
        std::vector<Token> fired;
        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == m_wakeFd) {
                woken = true;
            } else {
                collectFdWaits(events[i], fired);
            }
        }
        if (woken) {
            std::uint64_t count = 0;
            ssize_t r = ::read(m_wakeFd, &count, sizeof count);
            (void)r;
        }
        std::sort(fired.begin(), fired.end());
        for (Token token : fired) {
            dropFdWait(m_tokenFd.find(token)->second, token);
        }
        ready.insert(ready.end(), fired.begin(), fired.end());
        collectTimers(ready);
        if (woken || !ready.empty() || !block) {
            return;
        }
    }
}
