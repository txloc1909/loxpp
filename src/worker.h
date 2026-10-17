#pragma once

// The parts of a worker that another thread touches (spec/04-semantics.md,
// Workers and Tasks; docs/decisions/concurrency-model-decision.md). A worker
// is one OS thread with one Runtime and one MemoryManager. Everything in its
// heap is touched by that thread alone, except:
//
//   - the inbound queue below, under its mutex;
//   - the hold table of the heap (MemoryManager::hold), under its own lock;
//   - the fields of a message object, which nothing writes after the object
//     is built, and ObjTask::ended, an atomic;
//   - Obj::heapId, which nothing writes after MemoryManager::create.

#include "value.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class MemoryManager;
class Runtime;
struct ObjTask;

// --- Heap ids ---------------------------------------------------------------
//
// Each MemoryManager has an id that no other heap that exists now has. Obj::
// heapId names the heap that created the object, so any thread can find the
// owner of an object it holds, and a collector can tell a foreign object
// from one of its own with one compare. An id is free again when its heap is
// destroyed.

// Id 0 is never given: an Obj that no MemoryManager::create made keeps it.
std::uint16_t acquireHeapId(MemoryManager* heap);
void releaseHeapId(std::uint16_t id);
MemoryManager* heapById(std::uint16_t id);
inline MemoryManager* heapOf(const Obj* obj) { return heapById(obj->heapId); }

// --- The inbound queue -------------------------------------------------------

struct Envelope {
    ObjTask* target; // a task of the receiving worker
    Value msg;       // the sender raised a hold on it, if it is an object
};

class WorkerGroup;

// One per heap. A sender takes the lock, appends, releases, and signals. The
// receiving worker swaps the queue out under the lock between tasks.
struct Inbox {
    std::mutex mutex;
    std::condition_variable wake;
    // Guarded by `mutex`.
    std::vector<Envelope> queue;
    // Guarded by `mutex`. True while the worker waits on `wake` with no task
    // ready: it then does not count in WorkerGroup::busy.
    bool idle{false};
    // Guarded by `mutex`. True once the worker ended: a send then raises
    // DeadTaskError, and nothing is appended again.
    bool exited{false};
    // Guarded by `mutex`. The program run this worker belongs to now, and
    // the Runtime that runs on it (null once it ended). The deadlock report
    // reads `runtime` under `mutex`.
    std::shared_ptr<WorkerGroup> group;
    Runtime* runtime{nullptr};
    int workerNumber{0};
    // A hint that `queue` is not empty, so a worker reads one atomic between
    // two tasks and takes the lock only when there is work.
    std::atomic<bool> nonEmpty{false};
};

// Appends `env` to the queue of `target` and wakes its worker. Returns false,
// and appends nothing, when that worker ended.
bool postEnvelope(Inbox& target, const Envelope& env);

// --- One run of a program across its workers --------------------------------

// The workers of one run of a program: the main worker and every worker
// that its tasks spawned, directly or not. Shared by every Runtime of the
// run; it lives until the last of them is gone.
class WorkerGroup {
  public:
    // The number of workers that are not idle. Starts at one, for the main
    // worker. A worker that takes it to zero has seen that every worker
    // waits with nothing inbound, which is a deadlock (spec/04-semantics.md,
    // Deadlock): see goIdle().
    std::atomic<int> busy{1};
    // Set once by the worker that saw the deadlock. The main worker reports.
    std::atomic<bool> deadlock{false};
    // Set when the main task ended: every other task is abandoned, and each
    // worker leaves at its next scheduling point.
    std::atomic<bool> ended{false};

    Inbox* mainInbox{nullptr};

    // Guarded by `mutex`: the inbox of every worker of this run that has not
    // ended, the main worker's first.
    std::mutex mutex;
    std::vector<Inbox*> inboxes;
    int nextWorkerNumber{1};

    void addWorker(Inbox* inbox);
    void removeWorker(Inbox* inbox);

    // Called with `inbox.mutex` held and `inbox.queue` empty. Marks the
    // worker idle. Returns true when it was the last busy worker: then the
    // program is in deadlock, `deadlock` is set, and the main worker is
    // woken to report it.
    bool goIdle(Inbox& inbox);
    // A worker leaves the group for good (its tasks all ended). Returns true
    // when that makes every worker idle: a deadlock, as for goIdle().
    bool leave(Inbox& inbox);
    // The main task ended: wakes every worker so that it leaves.
    void end();

  private:
    void signalDeadlock();
};

// --- Ending the process -----------------------------------------------------

// Flushes stdout and stderr and ends the process with `code`. Safe to call
// from any worker, also from two at once: the first call ends the process,
// and a later one waits for that end.
[[noreturn]] void endProcess(int code);
