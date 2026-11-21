#pragma once

#include "collector.h"
#include "object.h"

#include <cstdint>
#include <cstdio>
#include <vector>

class MemoryManager;

// The stop-the-world mark-sweep collector, the reference collector of
// docs/decisions/garbage-collectors.md (section 3). It marks from the roots,
// traces every gray object, removes the unmarked Strings from the intern
// table, and sweeps the list of all objects, in one pause.
class StwCollector final : public Collector {
  public:
    // `workerNumber` names the worker of the heap; the GC trace of worker
    // N > 0 goes to `<LOXPP_GC_TRACE>.w<N>`.
    StwCollector(MemoryManager& heap, int workerNumber);
    // Writes the end record of the trace and closes it.
    ~StwCollector() override;

    void poll() override { collectNow(CollectCause::Auto); }
    void onCreate(Obj* obj) override { allObjects.push_back(obj); }
    void markObject(Obj* obj) override;
    void collectNow(CollectCause cause) override;
    [[nodiscard]] std::size_t collectionCount() const override {
        return m_collectionCount;
    }
    void releaseAll() override;

#ifdef LOXPP_PROFILE
    // Folds every live coroutine's profiler tables into the root profiler and
    // clears them, so the report sees coroutine work but a later report does
    // not count it again.
    void mergeCoroutineProfilers() override;
#endif

  private:
    template <bool Traced>
    void runCollection();
    void traceReferences();
    void removeWhiteStrings();
    void sweep();
    [[nodiscard]] bool isMarkedHere(const Obj* obj) const;
    // The `cause` field of a GC trace record.
    [[nodiscard]] const char* collectionCause() const;

    MemoryManager& m_heap;
    // Plain std::vector: a VmAllocator here would recurse into rawAlloc when
    // push_back reallocates.
    std::vector<Obj*> allObjects;
    std::vector<Obj*> m_grayStack;
    // Open when LOXPP_GC_TRACE names a file, read once when the heap starts.
    // Null otherwise, and a collection then pays one test of this pointer.
    // The allocation paths never read it.
    std::FILE* m_trace{nullptr};
    // The threshold never drops below kMinHeap. Without a floor, a program
    // with a small live heap and heavy allocation gets a threshold of a few
    // kilobytes and collects thousands of times. Pause time follows the
    // garbage swept, so a larger floor trades fewer collections for longer
    // pauses; one megabyte already removes the excess collections.
    static constexpr std::size_t kMinHeap = 1024 * 1024;
    static constexpr int GC_HEAP_GROW_FACTOR = 2;
    std::size_t m_collectionCount{0};
    bool m_exitCollection{false};
};
