#pragma once

// The wait set of one worker: descriptors it waits on, timers it waits for,
// and a wakeup that another thread can fire. This file and poller.cpp are the
// only place that uses epoll and eventfd, which are Linux calls: a port to
// another system replaces these two files and nothing else.
//
// Only the owner thread calls everything except wake(). wake() is the one
// call that any thread may make, while the owner is inside wait() or is about
// to enter it.

#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

struct epoll_event;

class Poller {
  public:
    using Clock = std::chrono::steady_clock;
    // Names one registered wait. Never 0, never reused by one Poller.
    using Token = std::uint64_t;
    enum class Interest : std::uint8_t { Read = 1, Write = 2 };

    Poller();
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    // Registers a wait for `fd` to be ready for `interest`. Level-triggered
    // and one-shot: the record is removed when it is reported. Returns 0, with
    // errno set, when epoll refuses the descriptor (a regular file, for one).
    // The caller must call cancelFd() before it closes `fd`.
    Token waitFd(int fd, Interest interest);
    // Registers a wait for `deadline`. One-shot.
    Token waitUntil(Clock::time_point deadline);
    // Removes a wait that was not reported yet. False when it is not there.
    bool cancel(Token token);
    // Removes every wait on `fd` and appends their tokens to `cancelled`, in
    // registration order. Call it before closing `fd`: epoll keeps its
    // reference to the open file description after close, and the number can
    // be given to a new descriptor.
    void cancelFd(int fd, std::vector<Token>& cancelled);

    // True while any wait is registered.
    [[nodiscard]] bool hasWaits() const {
        return !m_tokenFd.empty() || !m_timers.empty();
    }

    // Blocks until a wait is reported or wake() was called, and handles EINTR
    // and an early return of the timeout inside. `ready` is cleared, then
    // holds the reported tokens: the descriptor waits in registration order,
    // then the timers in deadline order. It is empty when only wake() ended
    // the call. With no wait registered, it returns after wake() only.
    void wait(std::vector<Token>& ready);

    // Makes a current or the next wait() return. Any thread. Sticky: a call
    // that comes before wait() makes that wait() return at once, so a wake
    // between the caller's last check and the wait is not lost.
    void wake() const;

  private:
    struct FdWait {
        Token token;
        Interest interest;
    };
    struct FdEntry {
        std::vector<FdWait> waits; // in registration order
        std::uint32_t mask{0};     // the events registered with epoll
    };

    void dropFdWait(int fd, Token token);
    [[nodiscard]] int nextTimeoutMs() const;
    void collectFdWaits(const epoll_event& ev, std::vector<Token>& fired) const;
    void collectTimers(std::vector<Token>& ready);

    int m_epoll{-1};
    int m_wakeFd{-1};
    Token m_nextToken{1};
    std::unordered_map<int, FdEntry> m_fds;
    std::unordered_map<Token, int> m_tokenFd; // fd waits only
    std::set<std::pair<Clock::time_point, Token>> m_timerQueue;
    std::unordered_map<Token, Clock::time_point> m_timers;
};
