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

Poller::Token Poller::waitFd(int fd, Interest interest, Token again) {
    FdEntry& entry = m_fds[fd];
    const bool fresh = entry.waits.empty();
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
    // Tokens grow with the first wait of a call, so this keeps registration
    // order for a call that waits again.
    entry.waits.insert(
        std::upper_bound(entry.waits.begin(), entry.waits.end(), token,
                         [](Token t, const FdWait& w) { return t < w.token; }),
        {token, interest});
    m_tokenFd.emplace(token, fd);
    return token;
}

Poller::Token Poller::waitUntil(Clock::time_point deadline) {
    const Token token = m_nextToken++;
    m_timerQueue.emplace(deadline, token);
    m_timers.emplace(token, deadline);
    return token;
}

void Poller::dropFdWait(int fd, Token token) {
    auto found = m_fds.find(fd);
    if (found == m_fds.end()) {
        return;
    }
    FdEntry& entry = found->second;
    entry.waits.erase(
        std::remove_if(entry.waits.begin(), entry.waits.end(),
                       [&](const FdWait& w) { return w.token == token; }),
        entry.waits.end());
    m_tokenFd.erase(token);
    if (entry.waits.empty()) {
        // EBADF and ENOENT: the caller closed the descriptor first. Nothing
        // is left to remove, so the failure is the same as the success.
        ::epoll_ctl(m_epoll, EPOLL_CTL_DEL, fd, nullptr);
        m_fds.erase(found);
        return;
    }
    std::uint32_t mask = 0;
    for (const FdWait& w : entry.waits) {
        mask |= eventBit(w.interest);
    }
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
    for (const FdWait& w : found->second.waits) {
        cancelled.push_back(w.token);
        m_tokenFd.erase(w.token);
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
    for (const FdWait& w : found->second.waits) {
        if (failed || (ev.events & eventBit(w.interest)) != 0) {
            fired.push_back(w.token);
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
