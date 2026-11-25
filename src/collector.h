#pragma once

// The interface between the runtime and a garbage collector
// (docs/decisions/garbage-collectors.md, section 5). A heap (MemoryManager)
// has one collector, chosen once when the heap starts (LOXPP_GC, section 5.1).
//
// The runtime gives the collector the roots (it calls markObject and
// markValue on them), the references of each object type (object_trace.h),
// and the allocation path. The collector gives the runtime the calls below.
// Only a collector reads or writes the mark of an object, and only a
// collector owns the list of the objects of a heap.

#include "object.h"
#include "value.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

class MemoryManager;

// The state that the allocation path reads on every call. It lives in the
// heap, next to bytesAllocated, so that the check is one compare of members
// of the heap and needs no load through a pointer to the collector. The
// collector writes it.
struct PollState {
    // The heap size at which the next collection starts. It starts above any
    // heap size, so no allocation polls before a collector has set it.
    std::size_t pollAt{~std::size_t{0}};
    // A non-empty LOXPP_STRESS_GC value asks for a collection at each
    // allocation. Read once, when the heap starts.
    bool stress{false};
    // True while the barriers must run. Only an incremental collector sets
    // it. The write barriers and the read barrier of the intern table
    // (decision 4.7) test it. The read barrier must also run during a sweep, so
    // that collector sets the flag for the sweep too, or adds a second flag.
    bool marking{false};
};

// Why a caller asks for a collection now. Auto is the cause that the
// collector would report itself (a threshold, or stress mode).
enum class CollectCause : std::uint8_t { Auto, Exit };

// The store kinds of section 5 that need a write barrier.
enum class Store : std::uint8_t { Field, Element, Entry, Upvalue, Snapshot };

class Collector {
  public:
    explicit Collector(PollState& poll) : m_poll(poll) {}
    virtual ~Collector() = default;

    // --- The allocation path -------------------------------------------

    // The heap decides that an allocation is a safe point at which the
    // collector has work: the heap size is above PollState::pollAt, or stress
    // mode is on. poll() then runs that work. A collection can run in the
    // middle of an allocation, so every caller of an allocation is a safe
    // point (docs/reference/gc-runtime-contract.md, section 2).
    virtual void poll() = 0;
    // The heap made `obj`. The collector registers it.
    virtual void onCreate(Obj* obj) = 0;
    // The file of the GC trace (LOXPP_GC_TRACE), or nullptr. The heap writes
    // its `stall` records there.
    [[nodiscard]] virtual std::FILE* traceFile() const { return nullptr; }

    // --- The roots -----------------------------------------------------

    // The runtime calls this for each root and each reference that it finds.
    virtual void markObject(Obj* obj) = 0;
    void markValue(const Value& v) {
        if (isObj(v)) {
            markObject(as<Obj*>(v));
        }
    }

    // --- Explicit collection and the end of a heap ---------------------

    // A full collection now (the exit of a worker, the idle point, tests).
    virtual void collectNow(CollectCause cause) = 0;
    [[nodiscard]] virtual std::size_t collectionCount() const = 0;
    // Frees every object of the heap. The destructor of the heap calls it.
    virtual void releaseAll() = 0;
#ifdef LOXPP_PROFILE
    // Folds the profiler tables of every live coroutine into the root
    // profiler and clears them.
    virtual void mergeCoroutineProfilers() {}
#endif

    // --- The write barriers (empty for the stop-the-world collector) ----
    //
    // The runtime calls one of these at each store of a reference into a heap
    // object, with the old and the new value. The fast path of each is one
    // test of a byte that the collector keeps ("is a mark in progress?").
    // The stop-the-world collector never sets it.

    // True while a barrier hook does work. A mutator reads the old value of a
    // store only when this is true (src/barrier.h).
    [[nodiscard]] bool marking() const { return m_poll.marking; }

    void barrierField(Obj* container, const Value& oldv, const Value& newv) {
        if (m_poll.marking) {
            onStore(Store::Field, container, oldv, newv);
        }
    }
    void barrierElement(Obj* container, const Value& oldv, const Value& newv) {
        if (m_poll.marking) {
            onStore(Store::Element, container, oldv, newv);
        }
    }
    void barrierEntry(Obj* container, const Value& oldv, const Value& newv) {
        if (m_poll.marking) {
            onStore(Store::Entry, container, oldv, newv);
        }
    }
    void barrierUpvalue(Obj* container, const Value& oldv, const Value& newv) {
        if (m_poll.marking) {
            onStore(Store::Upvalue, container, oldv, newv);
        }
    }
    void barrierSnapshot(Obj* container, const Value& oldv, const Value& newv) {
        if (m_poll.marking) {
            onStore(Store::Snapshot, container, oldv, newv);
        }
    }
    // The read barrier of the weak String table (section 4.7): a lookup of
    // the intern table found `s`.
    void internHit(ObjString* s) {
        if (m_poll.marking) {
            onInternHit(s);
        }
    }

  protected:
    virtual void onStore(Store, Obj*, const Value&, const Value&) {}
    virtual void onInternHit(ObjString*) {}

    PollState& m_poll;
};

// The collector that LOXPP_GC names, for `heap`. An unset or empty variable
// gives the default, the stop-the-world collector "stw". An unknown name
// prints a message and ends the process with status 64.
std::unique_ptr<Collector> makeCollector(MemoryManager& heap, PollState& poll,
                                         int workerNumber);
// The same for a name: nullptr for a name that is not a collector.
std::unique_ptr<Collector> makeCollectorByName(const char* name,
                                               MemoryManager& heap,
                                               PollState& poll,
                                               int workerNumber);
