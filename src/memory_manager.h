#pragma once

#include "collector.h"
#include "foreign_set.h"
#include "object.h"
#include "size_class_heap.h"
#include "stw_collector.h"
#include "table.h"
#include "vm_allocator.h"
#include "worker.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef LOXPP_PROFILE
struct ProfilerData; // defined in profiler.h, included only in
                     // memory_manager.cpp
#endif

class Compiler;

class MemoryManager : public VmAllocBase {
  public:
    // `workerNumber` names the worker the heap belongs to; the GC trace of
    // worker N > 0 goes to `<LOXPP_GC_TRACE>.w<N>`.
    explicit MemoryManager(int workerNumber = 0);
    ~MemoryManager();

    // Non-copyable, non-movable (VmAllocator stores 'this' pointer).
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;
    MemoryManager(MemoryManager&&) = delete;
    MemoryManager& operator=(MemoryManager&&) = delete;

    // Creates and takes ownership of a new Obj subclass.
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        // Stress mode of the collector fires a collection on every allocation
        // to surface rooting bugs. It is a runtime flag, not a build flag,
        // so this stays one member read and one `||` on the hot allocation
        // path.
        if (shouldPoll()) {
            m_collector->poll();
            // A step of the collector can drop the last count of a dead heap
            // (issue #1352). The part that destroys it runs here, after the
            // step, on this heap's thread.
            reapPendingPart();
        }
        static_assert(sizeof(T) <= SizeClassHeap::kMaxObject &&
                          alignof(T) <= SizeClassHeap::kSlotAlign,
                      "an object must fit a size class of the heap");
        bytesAllocated += sizeof(T);
        void* mem = m_store.allocObject(sizeof(T));
        T* p;
        try {
            p = new (mem) T(std::forward<Args>(args)...);
        } catch (...) {
            m_store.freeObject(mem, false);
            bytesAllocated -= sizeof(T);
            throw;
        }
        p->heapId = m_heapId;
        // The slot is an object of the heap only now. A collection that ran
        // inside the constructor did not see it.
        m_store.commitObject(p);
        // Every boxed Obj* goes through here. A pointer the NaN tag cannot
        // hold would be silently truncated by Value's constructor, so reject it
        // before it can be stored anywhere (src/value.h: pointerFitsInValue).
        if (!pointerFitsInValue(reinterpret_cast<uintptr_t>(p))) {
            reportPointerOutOfRange(reinterpret_cast<uintptr_t>(p));
        }
        // m_stw is the same object as m_collector when the collector is
        // "stw". StwCollector is final, so this is a direct call that the
        // compiler inlines: the allocation path pays no virtual call.
        if (m_stw != nullptr) {
            m_stw->onCreate(p);
        } else {
            m_collector->onCreate(p);
        }
        return p;
    }

    void release(Obj* obj, std::size_t size);

    // Returns an interned ObjString, creating one if not already present.
    ObjString* makeString(std::string_view sv);
    ObjString* makeString(std::string&& sv);
    // Takes the bytes with no copy when the String is new. Build sv with
    // VmAllocator<char>{this}: its allocation can collect, so the objects
    // that the bytes come from must stay rooted until sv is complete.
    ObjString* makeString(LoxString&& sv);
    ObjString* makeString(const char* chars) {
        return makeString(std::string_view{chars});
    }
    [[nodiscard]] ObjString* findString(std::string_view sv) const;

    // Frees every object of the heap.
    void collectAll() { m_collector->releaseAll(); }

    // The collector of this heap, chosen when the heap started (LOXPP_GC).
    [[nodiscard]] Collector& collector() { return *m_collector; }
    // For tests. The state that the allocation path and the barriers read.
    [[nodiscard]] PollState& pollState() { return m_poll; }
    // For tests. Puts `collector` in the place of the collector of this heap
    // and returns the old one. `collector` must use pollState(). An object
    // that the heap makes while `collector` is in place is not known to the
    // old collector, so a test makes none, and puts the old collector back
    // before the heap ends.
    std::unique_ptr<Collector>
    swapCollector(std::unique_ptr<Collector> collector);

    void* rawAlloc(std::size_t bytes) override;
    void rawFree(void* p, std::size_t bytes) noexcept override;

    // The memory of this heap: size classes and mark bitmaps. A collector
    // marks and sweeps through it.
    [[nodiscard]] SizeClassHeap& store() { return m_store; }

    void pushTempRoot(Obj* obj);
    void popTempRoot();

    void setMarkRootsCallback(std::function<void()> cb);
    void setCurrentCompiler(Compiler* c);
#ifdef LOXPP_PROFILE
    void setProfilerData(ProfilerData* data) { m_profilerData = data; }
    // Folds every live coroutine's profiler tables into the root profiler and
    // clears them, so the report sees coroutine work but a later report does
    // not count it again.
    void mergeCoroutineProfilers();
#endif
    // The roots and the references that the runtime finds go to the collector.
    void markObject(Obj* obj) { m_collector->markObject(obj); }
    void markValue(const Value& v) { m_collector->markValue(v); }
    void collectGarbage() {
        m_collector->collectNow(CollectCause::Auto);
        reapPendingPart();
    }
    // The number of completed collectGarbage() calls of this heap. Tests read
    // it to prove that an allocation path reaches the threshold check.
    [[nodiscard]] std::size_t collectionCount() const {
        return m_collector->collectionCount();
    }

    // The nodes of a message graph that the send and receive path of this
    // heap visited. That path hands over the root object only, so the count
    // per message must not depend on the size of the message. Code that walks
    // the graph of a message on this path calls noteMessageNode() for each
    // node it visits. Only the owner thread writes the count; a test reads it
    // after a reply that orders the read, as it does bytesAllocated.
    void noteMessageNode() { ++m_messageNodes; }
    [[nodiscard]] std::size_t messageNodesVisited() const {
        return m_messageNodes;
    }

    // --- Workers (docs/decisions/concurrency-model-decision.md, "Lifetime
    // across workers") ---------------------------------------------------
    //
    // A message object lives in the heap that created it. A worker that
    // holds a message of another heap counts once in the holder count of
    // that object, and an envelope on its way counts once too. The owner's
    // collector treats an object with a count above zero as a root. The
    // counts of one heap change under its hold lock, from any thread, and
    // the owner reads them all under that lock in one step at the start of a
    // collection. One step matters: a worker that reached G only through a
    // field of a held E raises G before it drops E, and the owner must not
    // see the drop of E without the raise of G.

    [[nodiscard]] std::uint16_t heapId() const { return m_heapId; }
    [[nodiscard]] int workerNumber() const { return m_workerNumber; }
    [[nodiscard]] Inbox& inbox() { return m_inbox; }

    // Raises the count of `obj`, an object of this heap. Any thread.
    void hold(const Obj* obj);
    // Drops the count of `obj`, an object of this heap. Any thread. Returns
    // true when that was the last count of a heap whose owner is dead: the
    // heap is then owed to the caller, which destroys it in parts with
    // reapPendingPart(), or drains it as it ends. Nobody may touch the heap
    // or its objects until the last part.
    bool releaseHold(const Obj* obj);
    // The holder count of `obj`, an object of this heap. For tests.
    [[nodiscard]] std::uint32_t holderCount(const Obj* obj);
    // hold()/releaseHold() on the owner of `v`, when `v` is an object.
    static void holdValue(const Value& v);
    // Drops the count of `v`, and takes the heap when that was its last one
    // (see releaseHold). A member, not a static, because the heap that calls
    // it owns the dead heap (m_destroying).
    void releaseValue(const Value& v);

    // --- The destruction of a dead heap (issue #1352) ----------------------
    //
    // The heap that drops the last count of a dead heap destroys it, in parts
    // of at most `units` work units. One unit is an object destroyed, a
    // foreign entry visited or an envelope released. A part is program work
    // and not a collector step, so it runs on this heap's own thread after a
    // step, in its idle time, or as it ends -- never inside a step.
    static constexpr std::size_t kDestroyUnits = 8192;
    // One part of the first dead heap of this heap. Returns its work units, or
    // 0 when no dead heap waits. A test calls it to count the parts.
    std::size_t reapPendingPart();
    // Runs parts for the idle time of this heap, about 0.5 ms: the limit that
    // the collector's own idle burst uses.
    void reapPendingIdle();
    static constexpr std::uint64_t kDestroyIdleNs = 500'000;
    // The dead heaps that wait for this heap, and the one part in progress.
    [[nodiscard]] std::size_t pendingDeadHeaps() const {
        return m_destroying.size() + (m_destroyingNow != nullptr ? 1 : 0);
    }
    // For tests: the units of one part. A test sets 1 and counts the parts.
    void setDestroyPartUnits(std::size_t units) { m_destroyUnits = units; }

    // A message that a send from another worker delivered to this heap: the
    // sender raised one count for it. Registers or releases that count, and
    // returns the value the receiver keeps:
    //   - an object of this heap, or one already in the foreign set: the
    //     count is released;
    //   - a String: intern on receive. The receiver probes its own intern
    //     table with the hash the string carries. A string with the same
    //     characters there is kept instead, and the count is released (the
    //     received one is not foreign here). Otherwise the received string
    //     becomes the interned one and goes into the foreign set;
    //   - any other new foreign object goes into the foreign set and keeps
    //     the count.
    Value receiveMessage(Value msg);
    // Adds `obj`, an object of another heap with one count already raised
    // for this heap, to the foreign set; releases that count when the set
    // has it already.
    void adoptForeign(Obj* obj);

    // The worker of this heap ended. Marks the inbox exited, releases every
    // message still in it, and runs one last collection whose only roots
    // are the held objects (GC trace cause=exit). The heap then holds
    // exactly the held messages and what they reach, and its foreign set
    // only the foreign objects those messages reach. No allocation may
    // follow.
    void retire();
    // The owner of the heap is gone. Frees the heap when no count is left;
    // otherwise it is a dead heap, and its last releaseHold() hands it to the
    // heap that dropped the count, which destroys it in parts.
    void dropOwner();

  private:
    // The collector reaches the roots, the foreign set and the intern table
    // of its heap through these calls and this friend declaration.
    friend class StwCollector;
    friend class IncrementalCollector;
    // Calls markObject and markValue of the collector for each root: the
    // root callback, the compiler, the temp roots, the held objects, the
    // inbox targets and the batch that the drain took out.
    void markAllRoots();
    // The foreign set at the start of a collection: no side mark is set, and
    // no foreign object is reached yet.
    void beginForeignCycle();
    // Sets the side mark of `obj`, an object of another heap.
    void markForeign(Obj* obj);
    [[nodiscard]] bool foreignMarked(const Obj* obj) const;
    void markHeldRoots();
    void markInboxTargets();
    // The roots that another thread can change while a mark runs: the held
    // objects, the inbox targets and the batch of the drain. An incremental
    // collector reads them again in its finish pause.
    void markFinishRoots();
    // Drops the count on each foreign object that the cycle did not reach.
    // With dropStrings, a String among them leaves the intern table first: a
    // collector that does not scan the table (decision 4.7) removes the
    // Strings of this heap when it frees them, and these are the others.
    void releaseUnreachedForeign(bool dropStrings = false);
    // The same in parts: begin, then call releaseForeignStep until
    // foreignReleasing is false. A step visits at most `maxEntries` entries
    // and returns the number that it visited. An entry that the program
    // marks or adopts between the parts stays. endForeignRelease stops the
    // scan; the entries that it did not visit stay in the set. `reaper` is
    // the heap that takes a heap whose last count the step drops: the
    // collector's own heap for a sweep, and the owning heap for the
    // destruction of a dead heap.
    void beginForeignRelease();
    std::size_t releaseForeignStep(std::size_t maxEntries, bool dropStrings,
                                   MemoryManager* reaper);
    [[nodiscard]] bool foreignReleasing() const;
    void endForeignRelease();
    [[nodiscard]] std::size_t foreignCount() const { return m_foreign.size(); }

    // Drops the count of `obj`, an object of another heap, and takes that
    // heap when it was its last one (releaseHold). For every count that this
    // heap drops.
    void releaseCount(Obj* obj);
    // One part of the destruction of the dead heap `this`. `reaper` takes a
    // heap whose last count the part drops. Sets `finished` when this heap is
    // empty, and returns the work units of the part.
    std::size_t destroyStep(MemoryManager& reaper, std::size_t units,
                            bool& finished);
    // Runs reapPendingPart() until no dead heap waits. The parts follow one
    // another on one thread, so no limit is needed here: the caller is
    // ending.
    void drainPendingDeadHeaps();

    // True when the allocation that is about to happen is a safe point at
    // which the collector has work. One compare of members of the heap.
    [[nodiscard]] bool shouldPoll() const {
        return m_poll.stress || bytesAllocated > m_poll.pollAt;
    }

    // The lookup of the intern table found `s`. The test of the flag is on the
    // state of the heap, so a lookup that finds a String (the common case)
    // pays one load, not three.
    void noteInternHit(ObjString* s) const {
        if (m_poll.internBarrier) {
            m_collector->internHit(s);
        }
    }

    // Declared first: every container of the heap frees its buffer into it,
    // so it must be destroyed last.
    SizeClassHeap m_store;

    Table m_strings;

    // The state that shouldPoll reads. Declared before the collector, which
    // holds a reference to it.
    PollState m_poll;
    std::unique_ptr<Collector> m_collector;
    StwCollector* m_stw{nullptr};
    std::vector<Obj*> m_tempRoots; // objects transiently protected from GC
    std::function<void()> m_markRoots;
    Compiler* m_currentCompiler{nullptr};
#ifdef LOXPP_PROFILE
    ProfilerData* m_profilerData{nullptr};
#endif
    std::size_t m_messageNodes{0};

    int m_workerNumber{0};
    std::uint16_t m_heapId{0};
    // The foreign set and the side mark table in one: the objects of other
    // heaps that this heap holds a count on, each with the side mark of the
    // collection in progress. The header mark bit of a foreign object belongs
    // to its owner's collector.
    ForeignSet m_foreign;
    // An envelope batch that the drain took out of the inbox and has not
    // delivered yet: the receiver's roots while it delivers.
    std::vector<Envelope> m_draining;
    Inbox m_inbox;

    // The holder counts of the objects of this heap, under m_holdMutex.
    std::mutex m_holdMutex;
    std::unordered_map<const Obj*, std::uint32_t> m_holders;
    std::size_t m_heldTotal{0};
    bool m_ownerAlive{true};

    // The dead heaps whose last count this heap dropped, and the one part
    // that runs now. Every count that this heap drops runs on its own thread,
    // so no lock is needed.
    std::vector<MemoryManager*> m_destroying;
    MemoryManager* m_destroyingNow{nullptr};
    std::size_t m_destroyUnits{kDestroyUnits};
    // The progress of destroyStep: the span cursor of the objects, the stage
    // (0 objects, 1 foreign set, 2 inbox), and the envelopes it took.
    SizeClassHeap::Span* m_destroySpan{nullptr};
    std::uint8_t m_destroyStage{0};
    bool m_destroyForeignStarted{false};
    bool m_destroyInboxTaken{false};
    std::vector<Envelope> m_destroyEnvelopes;

  public:
    // The drain's batch (see m_draining).
    std::vector<Envelope>& drainingBatch() { return m_draining; }
};

// Builds an Int value: inline when it fits and one heap box when it does not
// (docs/decisions/integer-kind.md, Native layout). This is the one place that
// decides, so the compiler, the VM's operand stack, and a native that returns
// an Int all agree, and an Int inside the inline range never allocates.
inline Value makeIntValue(MemoryManager& mm, Int value) {
    if (intFitsInline(value)) {
        return inlineIntValue(value);
    }
    return Value{static_cast<Obj*>(mm.create<ObjInt>(value))};
}
