#pragma once

#include "collector.h"
#include "object.h"
#include "seg_stack.h"
#include "size_class_heap.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unordered_set>

class MemoryManager;

// The incremental collector of docs/decisions/garbage-collectors.md (section
// 4): tri-color mark and sweep, non-moving, on the thread of the worker. A
// cycle is a start pause, mark steps, a finish pause, sweep steps and an end
// step. Each step has a fixed limit of work units. The program runs between
// the steps, and an allocation is the only place where a step can run.
//
// Colors. A set mark bit is gray or black, and the gray stack holds the gray
// objects. An object that the heap makes while a mark runs is black. An
// object that the heap makes while the sweep runs must live through this
// cycle and be white at the next one: it is marked when its span is not swept
// yet (SizeClassHeap::sweepEpoch tells), and left white otherwise. So the
// heap has no second white color and the header has no extra bit.
//
// The write barrier is a snapshot at the beginning (Yuasa): a store that
// replaces a reference makes the old target gray. The roots are read once, in
// the start pause.
class IncrementalCollector final : public Collector {
  public:
    enum class Phase : std::uint8_t { Idle, Mark, Sweep };

    IncrementalCollector(MemoryManager& heap, PollState& poll,
                         int workerNumber);
    ~IncrementalCollector() override;

    void poll() override;
    void onCreate(Obj* obj) override;
    [[nodiscard]] std::FILE* traceFile() const override { return m_trace; }
    void markObject(Obj* obj) override;
    void collectNow(CollectCause cause) override;
    [[nodiscard]] std::size_t collectionCount() const override {
        return m_collectionCount;
    }
    void releaseAll() override;
    // Finishes the cycle in progress, so that a quiet worker keeps no garbage
    // and no handle of the operating system.
    void onIdle() override;
#ifdef LOXPP_PROFILE
    void mergeCoroutineProfilers() override;
#endif

    // The limits of one step. A step stops at the first of: the units, or the
    // time. Counts are the main limit, because a count does not depend on the
    // host.
    static constexpr std::size_t kStepUnits = 8192;
    static constexpr std::uint64_t kStepNs = 100'000;
    // The units of one call of a trace of a large object, between two reads of
    // the clock.
    static constexpr std::size_t kChunkUnits = 64;
    // The share of the time that the collector may use while a cycle runs.
    static constexpr double kShare = 0.25;
    // A cycle starts when the heap reaches this factor of the heap after the
    // last cycle, and never below kMinHeap.
    static constexpr double kStartFactor = 1.5;
    // When the heap grows past this factor of its size at the start of the
    // cycle, the program no longer waits between steps.
    static constexpr double kEmergencyFactor = 1.5;
    static constexpr std::size_t kMinHeap = 1024 * 1024;
    // Bytes that the program allocates between two looks of the pacing.
    static constexpr std::size_t kPollBytes = 16 * 1024;
    // The units of a step in stress mode.
    static constexpr std::size_t kStressUnits = 16;

    // --- The steps, for tests and for the pacing -------------------------

    [[nodiscard]] Phase phase() const { return m_phase; }
    // The start pause: reads the roots. The phase must be Idle.
    void startCycle(const char* cause);
    // Marks at most about `units` units. True when the mark has nothing left
    // to trace (the cycle is ready for its finish pause).
    bool markStep(std::size_t units, std::uint64_t deadlineNs = 0);
    // The finish pause, then the phase is Sweep.
    void finishMark();
    // Sweeps at most about `units` slots. True when the sweep is over, and
    // the phase is Idle.
    bool sweepStep(std::size_t units, std::uint64_t deadlineNs = 0);
    // A full separate mark from the roots. Returns the number of reachable
    // objects of this heap that are white. It reads the roots, so it is for
    // the end of a mark.
    [[nodiscard]] std::size_t verify();
    // The largest number of units that one step has had.
    [[nodiscard]] std::size_t largestStepUnits() const {
        return m_largestStepUnits;
    }
    [[nodiscard]] std::size_t handleCount() const { return m_handles.size(); }

  protected:
    void onStore(Store, Obj* container, const Value& oldv,
                 const Value& newv) override;
    void onInternHit(ObjString* s) override;

  private:
    // What traceObjectEdges calls for each reference of an object. The calls
    // are direct, so the tracer pays no virtual call.
    struct Edges {
        IncrementalCollector& c;
        void markObject(Obj* o) { c.mark(o); }
        void markValue(const Value& v) {
            if (isObj(v)) {
                c.mark(as<Obj*>(v));
            }
        }
    };
    // A reference that the roots or the tracer found: counts one unit, then
    // makes the object gray (or, in the verifier, checks it).
    void mark(Obj* obj);
    void verifyVisit(Obj* obj);
    // Makes `obj` gray when it is an object of this heap, white and not
    // marked. The barrier and the roots use it.
    void shade(Obj* obj);
    // Traces at most `maxUnits` units, with a deadline in ns (0: none).
    // True when the gray stack and the current object are both empty.
    bool traceUntil(std::size_t maxUnits, std::uint64_t deadlineNs);
    // Traces up to kChunkUnits of m_cur. Sets m_cur to null when it is done.
    void traceChunk();

    bool grayEmpty() const { return m_cur == nullptr && m_gray.empty(); }
    void endCycle();
    void writeCycleRecord(std::uint64_t t1);
    void abortCycle();
    void runToCompletion();
    // One step of a paced cycle.
    void runStep(std::uint64_t deadlineNs, std::size_t units);
    void pace();
    [[nodiscard]] bool emergency() const;
    void setPhase(Phase p);
    void freeUnmarkedHandles();
    void freeDeadObject(Obj* obj);
    void logStep(const char* kind, std::uint64_t t0, std::uint64_t t1,
                 std::size_t units);
    void setNextPoll();

    MemoryManager& m_heap;
    SizeClassHeap& m_store;

    Phase m_phase{Phase::Idle};
    SegStack<Obj*> m_gray;
    // Objects that own a handle of the operating system (decision 4.5).
    SegStack<Obj*> m_handles;
    // The object that the tracer is inside, and where.
    Obj* m_cur{nullptr};
    std::size_t m_curPos{0};
    int m_curCap{-1};
    // Units of work done by the tracer and the roots, read as differences.
    std::size_t m_units{0};
    SizeClassHeap::Span* m_sweepCursor{nullptr};

    // The pacing.
    std::uint64_t m_nextStepNs{0};
    std::size_t m_cycleStartBytes{0};
    std::size_t m_pollBytes{0};

    // The cycle in progress.
    std::size_t m_cycleId{0};
    std::uint64_t m_cycleT0{0};
    std::size_t m_cycleBytesBefore{0};
    std::size_t m_cycleObjsBefore{0};
    std::size_t m_cycleSteps{0};
    std::size_t m_cycleFreed{0};
    const char* m_cycleCause{"threshold"};
    std::size_t m_largestStepUnits{0};
    std::size_t m_collectionCount{0};
#ifdef LOXPP_PROFILE
    std::int64_t m_cyclePauseNs{0};
    std::int64_t m_cycleMaxStepNs{0};
#endif
    // True while a collectNow runs the whole cycle with no program to wait
    // for.
    bool m_full{false};

    // The verifier. It runs at the end of each finish pause when it is on.
    bool m_verifyAtFinish{false};
    bool m_verifying{false};
    std::unordered_set<const Obj*> m_verifySeen;
    SegStack<Obj*> m_verifyStack;
    std::size_t m_verifyBad{0};

    std::FILE* m_trace{nullptr};
};
