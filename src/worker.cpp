#include "worker.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace {

constexpr std::size_t kHeapIdCount = 1U << 16;

// Never destroyed: a worker thread can still free a dead heap, and so free
// its id, while the process runs its exit handlers.
struct HeapRegistry {
    std::array<std::atomic<MemoryManager*>, kHeapIdCount> heaps{};
    std::mutex mutex;
    std::vector<std::uint16_t> freeIds;
    std::uint32_t nextId{1};
};

HeapRegistry& registry() {
    static auto* r = new HeapRegistry();
    return *r;
}

} // namespace

std::uint16_t acquireHeapId(MemoryManager* heap) {
    HeapRegistry& r = registry();
    std::uint16_t id = 0;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.freeIds.empty()) {
            id = r.freeIds.back();
            r.freeIds.pop_back();
        } else if (r.nextId < kHeapIdCount) {
            id = static_cast<std::uint16_t>(r.nextId++);
        }
    }
    if (id == 0) {
        std::fprintf(stderr,
                     "Too many heaps: at most %zu workers and heaps "
                     "can exist at once.\n",
                     kHeapIdCount - 1);
        endProcess(70);
    }
    r.heaps[id].store(heap, std::memory_order_release);
    return id;
}

void releaseHeapId(std::uint16_t id) {
    HeapRegistry& r = registry();
    r.heaps[id].store(nullptr, std::memory_order_release);
    std::lock_guard<std::mutex> lock(r.mutex);
    r.freeIds.push_back(id);
}

MemoryManager* heapById(std::uint16_t id) {
    return registry().heaps[id].load(std::memory_order_acquire);
}

bool postEnvelope(Inbox& target, const Envelope& env) {
    {
        std::lock_guard<std::mutex> lock(target.mutex);
        if (target.exited) {
            return false;
        }
        target.queue.push_back(env);
        target.nonEmpty.store(true, std::memory_order_release);
        if (target.idle) {
            // The sender is busy, so the count is above zero here and cannot
            // reach zero while this message is on its way.
            target.idle = false;
            target.group->busy.fetch_add(1);
        }
    }
    target.wake.notify_one();
    return true;
}

void WorkerGroup::addWorker(Inbox* inbox) {
    std::lock_guard<std::mutex> lock(mutex);
    inboxes.push_back(inbox);
}

void WorkerGroup::removeWorker(Inbox* inbox) {
    std::lock_guard<std::mutex> lock(mutex);
    inboxes.erase(std::remove(inboxes.begin(), inboxes.end(), inbox),
                  inboxes.end());
}

bool WorkerGroup::goIdle(Inbox& inbox) {
    inbox.idle = true;
    if (busy.fetch_sub(1) != 1) {
        return false;
    }
    if (&inbox != mainInbox) {
        signalDeadlock();
    } else {
        deadlock.store(true);
    }
    return true;
}

bool WorkerGroup::leave(Inbox& inbox) {
    bool wasIdle = false;
    {
        std::lock_guard<std::mutex> lock(inbox.mutex);
        wasIdle = inbox.idle;
        inbox.idle = false;
    }
    removeWorker(&inbox);
    if (wasIdle || busy.fetch_sub(1) != 1) {
        return false;
    }
    signalDeadlock();
    return true;
}

void WorkerGroup::signalDeadlock() {
    deadlock.store(true);
    // Take and drop the main worker's lock, so the store cannot fall between
    // its check of the wait condition and its wait.
    {
        std::lock_guard<std::mutex> lock(mainInbox->mutex);
    }
    mainInbox->wake.notify_all();
}

void WorkerGroup::end() {
    ended.store(true);
    std::lock_guard<std::mutex> lock(mutex);
    for (Inbox* inbox : inboxes) {
        {
            std::lock_guard<std::mutex> inner(inbox->mutex);
        }
        inbox->wake.notify_all();
    }
}

void endProcess(int code) {
    static std::atomic<bool> ending{false};
    if (ending.exchange(true)) {
        for (;;) {
            pause();
        }
    }
    // _Exit, not exit: another worker may still run, and exit() would
    // destroy static objects under it. Every stream is flushed first.
    std::fflush(nullptr);
    std::_Exit(code);
}
