#pragma once

#include "object.h"
#include "table.h"
#include "vm_allocator.h"
#include "worker.h"

#include <cstdint>
#include <cstdio>
#include <functional>
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
        // m_stressGC fires a collection on every allocation to surface rooting
        // bugs. It is a runtime flag, not a build flag, so this stays one
        // member read and one `||` on the hot allocation path.
        if (m_stressGC || bytesAllocated > m_nextGC) {
            collectGarbage();
        }
        bytesAllocated += sizeof(T);
        T* p = new T(std::forward<Args>(args)...);
        p->heapId = m_heapId;
        // Every boxed Obj* goes through here. A pointer the NaN tag cannot
        // hold would be silently truncated by Value's constructor, so reject it
        // before it can be stored anywhere (src/value.h: pointerFitsInValue).
        if (!pointerFitsInValue(reinterpret_cast<uintptr_t>(p))) {
            reportPointerOutOfRange(reinterpret_cast<uintptr_t>(p));
        }
        allObjects.push_back(p);
        return p;
    }

    void release(Obj* obj, std::size_t size);

    // Returns an interned ObjString, creating one if not already present.
    ObjString* makeString(std::string_view sv);
    ObjString* makeString(std::string&& sv);
    ObjString* makeString(const char* chars) {
        return makeString(std::string_view{chars});
    }
    [[nodiscard]] ObjString* findString(std::string_view sv) const;

    void collectAll();

    void* rawAlloc(std::size_t bytes) override;

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
    void markObject(Obj* obj);
    void markValue(const Value& v);
    void collectGarbage();
    // The number of completed collectGarbage() calls of this heap. Tests read
    // it to prove that an allocation path reaches the threshold check.
    [[nodiscard]] std::size_t collectionCount() const {
        return m_collectionCount;
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
    // Drops the count of `obj`, an object of this heap. Any thread. Frees
    // this heap when it is dead and that was its last count; the caller must
    // not touch the heap or its objects after that.
    void releaseHold(const Obj* obj);
    // The holder count of `obj`, an object of this heap. For tests.
    [[nodiscard]] std::uint32_t holderCount(const Obj* obj);
    // hold()/releaseHold() on the owner of `v`, when `v` is an object.
    static void holdValue(const Value& v);
    static void releaseValue(const Value& v);

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
    // otherwise it is a dead heap, and the last releaseHold() frees it.
    void dropOwner();

  private:
    template <bool Traced>
    void runCollection();
    void traceReferences();
    void traceObject(Obj* obj);
    void removeWhiteStrings();
    void sweep();
    void markHeldRoots();
    void markInboxTargets();
    void adoptReachedForeign();
    void releaseUnreachedForeign();
    [[nodiscard]] bool isMarkedHere(const Obj* obj) const;
    // The `cause` field of a GC trace record.
    [[nodiscard]] const char* collectionCause() const;

    // Plain std::vector — using VmAllocator here would cause infinite recursion
    // inside rawAlloc when push_back triggers a reallocation.
    std::vector<Obj*> allObjects;
    Table m_strings;

    std::vector<Obj*> m_grayStack;
    std::vector<Obj*> m_tempRoots; // objects transiently protected from GC
    std::function<void()> m_markRoots;
    Compiler* m_currentCompiler{nullptr};
#ifdef LOXPP_PROFILE
    ProfilerData* m_profilerData{nullptr};
#endif
    // Set from the LOXPP_STRESS_GC environment variable, read once in the
    // constructor. When true, every allocation triggers a full collection.
    // When false, m_nextGC growth and the trigger point are exactly as they
    // are with no stress: this flag adds one branch and nothing else.
    bool m_stressGC{false};
    // Open when LOXPP_GC_TRACE names a file, read once in the constructor.
    // Null otherwise, and collectGarbage() then pays one test of this pointer
    // per collection. The allocation paths never read it.
    std::FILE* m_trace{nullptr};
    // The threshold never drops below kMinHeap. Without a floor, a program
    // with a small live heap and heavy allocation gets a threshold of a few
    // kilobytes and collects thousands of times. Pause time follows the
    // garbage swept, so a larger floor trades fewer collections for longer
    // pauses; one megabyte already removes the excess collections.
    static constexpr std::size_t kMinHeap = 1024 * 1024;
    static constexpr int GC_HEAP_GROW_FACTOR = 2;
    std::size_t m_nextGC{kMinHeap};
    std::size_t m_collectionCount{0};

    int m_workerNumber{0};
    std::uint16_t m_heapId{0};
    // The foreign set and the side mark table in one: the keys are the
    // objects of other heaps that this heap holds a count on, and the value
    // is the side mark of the collection in progress. The header mark bit of
    // a foreign object belongs to its owner's collector.
    std::unordered_map<Obj*, bool> m_foreign;
    // Foreign objects the collection in progress reached that were not in
    // the set yet. The collection raises their counts before it releases
    // any count.
    std::vector<Obj*> m_reachedForeign;
    // An envelope batch that the drain took out of the inbox and has not
    // delivered yet: the receiver's roots while it delivers.
    std::vector<Envelope> m_draining;
    bool m_exitCollection{false};
    Inbox m_inbox;

    // The holder counts of the objects of this heap, under m_holdMutex.
    std::mutex m_holdMutex;
    std::unordered_map<const Obj*, std::uint32_t> m_holders;
    std::size_t m_heldTotal{0};
    bool m_ownerAlive{true};

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
