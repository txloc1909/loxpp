#include "incremental_collector.h"
#include "memory_manager.h"
#include "object_trace.h"
#include "table.h"

#ifdef LOXPP_PROFILE
#include "profiler.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {

constexpr std::size_t kNoLimit = std::numeric_limits<std::size_t>::max();

bool verifyEnabled() {
    const char* v = std::getenv("LOXPP_GC_VERIFY");
    if (v != nullptr && v[0] != '\0') {
        return v[0] != '0';
    }
#ifndef NDEBUG
    return true;
#else
    return false;
#endif
}

bool ownsHandle(ObjType t) {
    return t == ObjType::FILE || t == ObjType::SOCKET || t == ObjType::SERVER ||
           t == ObjType::PROCESS;
}

// An object that holds no reference. The tracer has nothing to do for it, so
// it never goes on the gray stack.
bool isLeaf(ObjType t) {
    return t == ObjType::STRING || t == ObjType::INT || t == ObjType::NATIVE;
}

} // namespace

IncrementalCollector::IncrementalCollector(MemoryManager& heap, PollState& poll,
                                           int workerNumber)
    : Collector(poll), m_heap(heap), m_store(heap.store()), m_gray(m_store),
      m_handles(m_store), m_verifyStack(m_store),
      m_verifyAtFinish(verifyEnabled()),
      m_trace(openGcTraceFile(workerNumber)) {
    m_poll.pollAt = kMinHeap;
}

IncrementalCollector::~IncrementalCollector() {
    if (m_trace != nullptr) {
        std::fprintf(m_trace, "end t=%llu\n",
                     static_cast<unsigned long long>(gcMonotonicNs()));
        std::fclose(m_trace);
    }
}

void IncrementalCollector::setPhase(Phase p) {
    m_phase = p;
    m_poll.marking = p == Phase::Mark;
    m_poll.internBarrier = p != Phase::Idle;
}

void IncrementalCollector::logStep(const char* kind, std::uint64_t t0,
                                   std::uint64_t t1, std::size_t units) {
    ++m_cycleSteps;
    m_largestStepUnits = std::max(m_largestStepUnits, units);
#ifdef LOXPP_PROFILE
    m_cyclePauseNs += static_cast<std::int64_t>(t1 - t0);
    m_cycleMaxStepNs =
        std::max(m_cycleMaxStepNs, static_cast<std::int64_t>(t1 - t0));
#endif
    if (m_trace != nullptr) {
        std::fprintf(m_trace,
                     "step cycle=%zu kind=%s t0=%llu t1=%llu units=%zu\n",
                     m_cycleId, kind, static_cast<unsigned long long>(t0),
                     static_cast<unsigned long long>(t1), units);
    }
}

// --- Roots, barriers and the read barrier ----------------------------------

void IncrementalCollector::shade(Obj* obj) {
    if (obj == nullptr) {
        return;
    }
    if (obj->heapId != m_heap.heapId()) {
        // An object of another heap: the heap marks it in its side table only,
        // and does not trace it. Its owner traces it while this heap holds it.
        if (obj->heapId == 0) {
            return;
        }
        m_heap.markForeign(obj);
        return;
    }
    if (m_store.tryMark(obj) && !isLeaf(obj->type)) {
        m_gray.push(obj);
    }
}

void IncrementalCollector::mark(Obj* obj) {
    if (obj == nullptr) {
        return;
    }
    ++m_units;
    if (m_verifying) [[unlikely]] {
        verifyVisit(obj);
        return;
    }
    shade(obj);
}

void IncrementalCollector::markObject(Obj* obj) { mark(obj); }

void IncrementalCollector::onStore(Store, Obj*, const Value& oldv,
                                   const Value&) {
    // Snapshot at the beginning (decision 4.3): the old target was reachable
    // when the mark started, so the collector keeps it.
    if (isObj(oldv)) {
        shade(as<Obj*>(oldv));
    }
}

void IncrementalCollector::onInternHit(ObjString* s) {
    if (s->heapId != m_heap.heapId()) {
        // A foreign String is kept or dropped in the finish pause, with the
        // other foreign objects.
        if (m_phase == Phase::Mark) {
            shade(s);
        }
        return;
    }
    // A String is a leaf: marking it is all the work. In a sweep the mark
    // keeps a String that the sweep has not reached yet, and the sweep clears
    // it when it gets there. A String in a swept span is white for the next
    // cycle already.
    if (m_phase == Phase::Mark || m_store.isUnswept(s)) {
        m_store.tryMark(s);
    }
}

void IncrementalCollector::onCreate(Obj* obj) {
    if (ownsHandle(obj->type)) {
        m_handles.push(obj);
    }
    if (m_phase == Phase::Mark) {
        m_store.tryMark(obj);
    } else if (m_phase == Phase::Sweep && m_store.isUnswept(obj)) {
        m_store.tryMark(obj);
    }
}

// --- The verifier ------------------------------------------------------------

void IncrementalCollector::verifyVisit(Obj* obj) {
    if (obj->heapId != m_heap.heapId()) {
        return;
    }
    if (!m_verifySeen.insert(obj).second) {
        return;
    }
    if (!m_store.isMarked(obj)) {
        ++m_verifyBad;
        std::fprintf(stderr,
                     "GC verifier: a reachable object is white: %p, type %d\n",
                     static_cast<void*>(obj), static_cast<int>(obj->type));
    }
    if (!isLeaf(obj->type)) {
        m_verifyStack.push(obj);
    }
}

std::size_t IncrementalCollector::verify() {
    m_verifying = true;
    m_verifyBad = 0;
    m_verifySeen.clear();
    const std::size_t units = m_units;
    m_heap.markAllRoots();
    Edges edges{*this};
    while (!m_verifyStack.empty()) {
        traceObjectEdges(m_verifyStack.pop(), edges);
    }
    m_verifying = false;
    m_verifySeen.clear();
    m_units = units;
    return m_verifyBad;
}

// --- The tracer --------------------------------------------------------------

void IncrementalCollector::traceChunk() {
    Obj* obj = m_cur;
    Edges e{*this};
    switch (obj->type) {
    case ObjType::LIST: {
        const auto& v = static_cast<ObjList*>(obj)->elements();
        const std::size_t n = v.size();
        const std::size_t end = std::min(n, m_curPos + kChunkUnits);
        for (std::size_t i = m_curPos; i < end; ++i) {
            e.markValue(v[i]);
        }
        m_curPos = end;
        if (end >= n) {
            m_cur = nullptr;
        }
        return;
    }
    case ObjType::MAP: {
        auto* map = static_cast<ObjMap*>(obj);
        const auto& t = map->entries();
        const int cap = t.capacity();
        // A rehash moves entries, so the part that is traced is not the part
        // that was: start again from the first bucket.
        if (m_curCap != cap) {
            m_curCap = cap;
            m_curPos = 0;
        }
        if (m_curPos == 0) {
            e.markObject(map->klass);
        }
        const int from = static_cast<int>(m_curPos);
        const int next =
            t.forEachRange(from, from + static_cast<int>(kChunkUnits),
                           [&e](const MapEntry& en) {
                               e.markValue(en.key);
                               e.markValue(en.value);
                           });
        m_units += static_cast<std::size_t>(next - from);
        m_curPos = static_cast<std::size_t>(next);
        if (next >= cap) {
            m_cur = nullptr;
        }
        return;
    }
    case ObjType::INSTANCE:
    case ObjType::CLASS: {
        const Table* t;
        if (obj->type == ObjType::INSTANCE) {
            auto* inst = static_cast<ObjInstance*>(obj);
            t = &inst->fields();
            if (m_curCap < 0) {
                e.markObject(inst->klass);
            }
        } else {
            auto* klass = static_cast<ObjClass*>(obj);
            t = &klass->methods();
            if (m_curCap < 0) {
                e.markObject(klass->name);
                e.markObject(klass->superclass());
            }
        }
        const int cap = t->capacity();
        // The first call has m_curCap -1. A rehash starts again.
        if (m_curCap != cap) {
            m_curCap = cap;
            m_curPos = 0;
        }
        const int from = static_cast<int>(m_curPos);
        const int next =
            t->forEachRange(from, from + static_cast<int>(kChunkUnits),
                            [&e](ObjString* k, const Value& v) {
                                e.markObject(k);
                                e.markValue(v);
                            });
        m_units += static_cast<std::size_t>(next - from);
        m_curPos = static_cast<std::size_t>(next);
        if (next >= cap) {
            m_cur = nullptr;
        }
        return;
    }
    case ObjType::COROUTINE: {
        auto* co = static_cast<ObjCoroutine*>(obj);
        if (m_curCap < 0) {
            m_curCap = 0;
            e.markObject(co->klass);
            e.markValue(co->callee);
            for (const auto& frame : co->frames()) {
                e.markObject(frame.closure);
            }
            for (const auto& deferList : co->defers()) {
                for (const auto& v : deferList) {
                    e.markValue(v);
                }
            }
            for (ObjUpvalue* uv : co->openUpvalues()) {
                e.markObject(uv);
            }
            for (const auto& v : co->resultOverrides()) {
                e.markValue(v);
            }
            if (co->waitingInner() != nullptr) {
                e.markObject(co->waitingInner());
            }
            if (co->waitingIterator() != nullptr) {
                e.markObject(co->waitingIterator());
            }
        }
        const auto& v = co->stack();
        const std::size_t n = v.size();
        const std::size_t end = std::min(n, m_curPos + kChunkUnits);
        for (std::size_t i = m_curPos; i < end; ++i) {
            e.markValue(v[i]);
        }
        m_curPos = end;
        if (end >= n) {
            m_cur = nullptr;
        }
        return;
    }
    case ObjType::TASK: {
        auto* task = static_cast<ObjTask*>(obj);
        if (m_curCap < 0) {
            m_curCap = 0;
            if (task->co() != nullptr) {
                e.markObject(task->co());
            }
            e.markValue(task->startArg());
            e.markValue(task->pendingValue());
            e.markValue(task->selectWant());
            if (task->io().target != nullptr) {
                e.markObject(task->io().target);
            }
            if (task->io().text != nullptr) {
                e.markObject(task->io().text);
            }
            m_curAux = task->poppedCount();
        }
        // A receive since the last part moved each message that stays one
        // place to the front, so the traced part is shorter by that count.
        const std::uint64_t popped = task->poppedCount();
        const std::uint64_t moved = popped - m_curAux;
        m_curPos =
            moved >= m_curPos ? 0 : m_curPos - static_cast<std::size_t>(moved);
        m_curAux = popped;
        const auto& box = task->mailbox();
        const std::size_t n = box.size();
        const std::size_t end = std::min(n, m_curPos + kChunkUnits);
        for (std::size_t i = m_curPos; i < end; ++i) {
            e.markValue(box[i]);
        }
        m_curPos = end;
        if (end >= n) {
            m_cur = nullptr;
        }
        return;
    }
    default:
        traceObjectEdges(obj, e);
        m_cur = nullptr;
        return;
    }
}

bool IncrementalCollector::traceUntil(std::size_t maxUnits,
                                      std::uint64_t deadlineNs) {
    const std::size_t base = m_units;
    std::size_t nextClock = kChunkUnits;
    for (;;) {
        if (m_cur == nullptr) {
            if (m_gray.empty()) {
                return true;
            }
            m_cur = m_gray.pop();
            m_curPos = 0;
            m_curCap = -1;
            ++m_units;
        }
        traceChunk();
        const std::size_t used = m_units - base;
        if (used >= maxUnits) {
            return grayEmpty();
        }
        if (deadlineNs != 0 && used >= nextClock) {
            nextClock = used + kChunkUnits;
            if (gcMonotonicNs() >= deadlineNs) {
                return grayEmpty();
            }
        }
    }
}

// --- The cycle ---------------------------------------------------------------

void IncrementalCollector::startCycle(const char* cause) {
    const std::uint64_t t0 = gcMonotonicNs();
    ++m_cycleId;
    m_cycleCause = cause;
    m_cycleT0 = t0;
    m_cycleBytesBefore = m_heap.bytesAllocated;
    m_cycleObjsBefore = m_store.objectCount();
    m_cycleSteps = 0;
    m_cycleFreed = 0;
    m_cycleFreedBytes = 0;
#ifdef LOXPP_PROFILE
    m_cyclePauseNs = 0;
    m_cycleMaxStepNs = 0;
#endif
    const std::size_t u0 = m_units;
    m_heap.beginForeignCycle();
    m_heap.markAllRoots();
    // The mark runs from now: an allocation is black and a store is a barrier.
    setPhase(Phase::Mark);
    m_cycleStartBytes = m_heap.bytesAllocated;
    m_cyclePeakBytes = m_cycleStartBytes;
    m_emergencyAt = static_cast<std::size_t>(
        static_cast<double>(m_cycleStartBytes) * kEmergencyFactor);
    const std::uint64_t t1 = gcMonotonicNs();
    logStep("start", t0, t1, m_units - u0);
    m_nextStepNs =
        t1 + static_cast<std::uint64_t>(static_cast<double>(t1 - t0) *
                                        (1.0 - kShare) / kShare);
}

bool IncrementalCollector::markStep(std::size_t units,
                                    std::uint64_t deadlineNs) {
    const std::uint64_t t0 = gcMonotonicNs();
    const std::size_t u0 = m_units;
    const bool done = traceUntil(units, deadlineNs);
    // A call that finds nothing to trace is not a pause worth a record.
    if (m_units != u0) {
        logStep("mark", t0, gcMonotonicNs(), m_units - u0);
    }
    return done;
}

void IncrementalCollector::freeUnmarkedHandles() {
    // The marks of this cycle are complete, so an unmarked handle object is
    // unreachable. Its destructor closes the handle of the operating system.
    const std::size_t bytes0 = m_heap.bytesAllocated;
    SegStack<Obj*> keep(m_store);
    while (!m_handles.empty()) {
        Obj* obj = m_handles.pop();
        ++m_units;
        if (m_store.isMarked(obj)) {
            keep.push(obj);
            continue;
        }
        ++m_cycleFreed;
        m_heap.release(obj, objAllocatedSize(obj));
    }
    m_handles.swap(keep);
    m_cycleFreedBytes += bytes0 - m_heap.bytesAllocated;
}

void IncrementalCollector::finishMark() {
    const std::uint64_t t0 = gcMonotonicNs();
    const std::size_t u0 = m_units;
    // A worker can have raised a hold, or sent an envelope, since the start.
    m_heap.markFinishRoots();
    traceUntil(kNoLimit, 0);
    if (m_verifyAtFinish && verify() != 0) {
        std::fprintf(stderr,
                     "GC verifier: the mark missed a reachable object\n");
        std::abort();
    }
    // Raise the counts of the foreign objects that this cycle reached before
    // any release: one of them can be reachable only through an object that
    // the release drops.
    m_heap.adoptReachedForeign();
    m_units += m_heap.foreignCount();
    freeUnmarkedHandles();
    m_heap.releaseUnreachedForeign(true);
    m_store.beginSweep();
    m_sweepCursor = m_store.firstObjectSpan();
    setPhase(Phase::Sweep);
    logStep("finish", t0, gcMonotonicNs(), m_units - u0);
}

void IncrementalCollector::freeDeadObject(Obj* obj) {
    m_heap.bytesAllocated -= objAllocatedSize(obj);
    ++m_cycleFreed;
    switch (obj->type) {
    case ObjType::STRING:
        // The table holds a String weakly, and the sweep is where the String
        // leaves it (decision 4.7).
        m_heap.m_strings.del(static_cast<ObjString*>(obj));
        break;
    case ObjType::FILE:
    case ObjType::SOCKET:
    case ObjType::SERVER:
    case ObjType::PROCESS:
        // The handle list holds the object, and the finish pause frees it.
        // The sweep must never free one: the list would keep a dead pointer.
        std::fprintf(stderr, "BUG: the sweep reached a handle object\n");
        std::abort();
    case ObjType::COROUTINE:
#ifdef LOXPP_PROFILE
        // An abandoned coroutine is only reachable through its snapshot; this
        // is the last chance to fold its profile into the report.
        if (m_heap.m_profilerData) {
            auto* co = static_cast<ObjCoroutine*>(obj);
            if (co->profiler) {
                m_heap.m_profilerData->mergeFrom(*co->profiler);
            }
        }
#endif
        break;
    default:
        break;
    }
}

bool IncrementalCollector::sweepStep(std::size_t units,
                                     std::uint64_t deadlineNs) {
    const std::uint64_t t0 = gcMonotonicNs();
    const std::size_t bytes0 = m_heap.bytesAllocated;
    std::size_t done = 0;
    while (m_sweepCursor != nullptr) {
        SizeClassHeap::Span* s = m_sweepCursor;
        if (m_store.spanSwept(s)) {
            m_sweepCursor = SizeClassHeap::nextObjectSpan(s);
            if (++done >= units) {
                break;
            }
            continue;
        }
        const std::size_t slots = std::max<std::size_t>(s->bump, 1);
        if (done > 0 && done + slots > units) {
            break;
        }
        m_sweepCursor = SizeClassHeap::nextObjectSpan(s);
        m_store.sweepSpan(s, [this](Obj* obj) { freeDeadObject(obj); });
        // A freed large block goes back to the kernel one slice for each span.
        m_store.releaseStep();
        done += slots;
        if (deadlineNs != 0 && gcMonotonicNs() >= deadlineNs) {
            break;
        }
    }
    m_cycleFreedBytes += bytes0 - m_heap.bytesAllocated;
    const bool over = m_sweepCursor == nullptr;
    logStep("sweep", t0, gcMonotonicNs(), done);
    if (over) {
        endCycle();
    }
    return over;
}

void IncrementalCollector::endCycle() {
    const std::uint64_t t0 = gcMonotonicNs();
    setPhase(Phase::Idle);
    // The objects that the program made during the cycle are black, so they
    // are in the heap after the sweep, and most of them can be garbage. The
    // next cycle starts from the heap that the cycle found live, not from the
    // heap that it left.
    m_liveBytes = m_cycleStartBytes > m_cycleFreedBytes
                      ? m_cycleStartBytes - m_cycleFreedBytes
                      : 0;
    m_poll.pollAt =
        std::max({static_cast<std::size_t>(static_cast<double>(m_liveBytes) *
                                           kStartFactor),
                  m_heap.bytesAllocated, kMinHeap});
    ++m_collectionCount;
    const std::uint64_t t1 = gcMonotonicNs();
    logStep("end", t0, t1, 0);
#ifdef LOXPP_PROFILE
    // One cycle is one collection of the profile. Its pause is the sum of its
    // steps, and its longest pause is its longest step.
    if (m_heap.m_profilerData) {
        GcStats& gc = m_heap.m_profilerData->gc;
        const std::size_t after = m_heap.bytesAllocated;
        gc.gcCount++;
        gc.totalPauseNs += m_cyclePauseNs;
        gc.maxPauseNs = std::max(gc.maxPauseNs, m_cycleMaxStepNs);
        gc.totalBytesFreed +=
            m_cycleBytesBefore > after ? m_cycleBytesBefore - after : 0;
        gc.peakBytesAllocated =
            std::max(gc.peakBytesAllocated, m_cycleBytesBefore);
        if (gc.events.size() < 1024) {
            gc.events.push_back({m_cycleBytesBefore, after, m_cyclePauseNs});
        }
    }
#endif
    writeCycleRecord(t1);
}

void IncrementalCollector::writeCycleRecord(std::uint64_t t1) {
    if (m_trace != nullptr) {
        const std::size_t objs = m_store.objectCount();
        std::fprintf(m_trace,
                     "cycle cause=%s t0=%llu t1=%llu steps=%zu "
                     "bytes_before=%zu bytes_after=%zu objs_before=%zu "
                     "objs_after=%zu marked=%zu freed=%zu "
                     "bytes_peak=%zu bytes_live=%zu\n",
                     m_cycleCause, static_cast<unsigned long long>(m_cycleT0),
                     static_cast<unsigned long long>(t1), m_cycleSteps,
                     m_cycleBytesBefore, m_heap.bytesAllocated,
                     m_cycleObjsBefore, objs, objs, m_cycleFreed,
                     m_cyclePeakBytes, m_liveBytes);
        // Flushed once for each cycle, so that a process that stops without
        // stdio cleanup still leaves every cycle so far.
        std::fflush(m_trace);
    }
}

void IncrementalCollector::abortCycle() {
    if (m_phase != Phase::Idle && m_trace != nullptr) {
        // The trace shows a cycle that a full collection cuts short as a cycle
        // that ends here: its steps so far, an end step and a cycle record.
        const std::uint64_t t = gcMonotonicNs();
        logStep("end", t, t, 0);
        writeCycleRecord(t);
    }
    while (!m_gray.empty()) {
        m_gray.pop();
    }
    m_cur = nullptr;
    // Every foreign object that the mark put in the set needs its count, or
    // the next cycle would release a count that nobody raised.
    m_heap.adoptReachedForeign();
    m_store.clearAllMarks();
    m_sweepCursor = nullptr;
    setPhase(Phase::Idle);
}

void IncrementalCollector::runToCompletion() {
    if (m_phase == Phase::Mark) {
        while (!markStep(kNoLimit)) {
        }
        finishMark();
    }
    if (m_phase == Phase::Sweep) {
        while (!sweepStep(kNoLimit)) {
        }
    }
}

void IncrementalCollector::collectNow(CollectCause cause) {
    // A cycle in progress keeps what died after its start, and a full
    // collection must not. So it starts again.
    if (m_phase != Phase::Idle) {
        abortCycle();
    }
    startCycle(cause == CollectCause::Exit
                   ? "exit"
                   : (m_poll.stress ? "stress" : "threshold"));
    runToCompletion();
}

void IncrementalCollector::onIdle() {
    // A worker that waits has no program to delay, but the wait can be short
    // (a timer of a millisecond), and a request can arrive at any time. So
    // the idle time is used in one burst of steps with a time limit, and the
    // next idle time goes on. A cycle that must end at once is the job of
    // collectNow.
    const std::uint64_t end = gcMonotonicNs() + kIdleBurstNs;
    while (m_phase != Phase::Idle && gcMonotonicNs() < end) {
        runStep(0, kStepUnits);
    }
}

void IncrementalCollector::releaseAll() {
    abortCycle();
    m_handles.clear();
    m_store.destroyAllObjects();
    m_heap.bytesAllocated = 0;
}

// --- The pacing ------------------------------------------------------------

bool IncrementalCollector::emergency() const {
    return m_heap.bytesAllocated > m_emergencyAt;
}

void IncrementalCollector::setNextPoll() {
    m_poll.pollAt = m_heap.bytesAllocated + kPollBytes;
}

void IncrementalCollector::runStep(std::uint64_t deadlineNs,
                                   std::size_t units) {
    if (m_phase == Phase::Mark) {
        if (grayEmpty()) {
            finishMark();
        } else {
            markStep(units, deadlineNs);
        }
    } else {
        sweepStep(units, deadlineNs);
    }
}

void IncrementalCollector::pace() {
    m_cyclePeakBytes = std::max(m_cyclePeakBytes, m_heap.bytesAllocated);
    const std::uint64_t now = gcMonotonicNs();
    const bool urgent = emergency();
    if (now < m_nextStepNs) {
        setNextPoll();
        return;
    }
    runStep(now + kStepNs, kStepUnits);
    if (m_phase == Phase::Idle) {
        return;
    }
    const std::uint64_t end = gcMonotonicNs();
    // The program runs until the collector's share of the time is back at the
    // target. In an emergency the target is larger, but it stays under one
    // half, so that the program still gets the larger part of each window.
    const double share = urgent ? kUrgentShare : kShare;
    const double gap = static_cast<double>(end - now) * (1.0 - share) / share;
    m_nextStepNs = end + static_cast<std::uint64_t>(gap);
    setNextPoll();
}

void IncrementalCollector::poll() {
    if (m_poll.stress) {
        // Stress mode: a very small step at each allocation, so that the
        // program and the collector mix as often as possible.
        if (m_phase == Phase::Idle) {
            startCycle("stress");
        } else {
            runStep(0, kStressUnits);
        }
        return;
    }
    if (m_phase == Phase::Idle) {
        startCycle("threshold");
        setNextPoll();
        return;
    }
    pace();
}

#ifdef LOXPP_PROFILE
void IncrementalCollector::mergeCoroutineProfilers() {
    if (!m_heap.m_profilerData) {
        return;
    }
    m_store.forEachObject([this](Obj* obj, bool) {
        if (obj->type != ObjType::COROUTINE) {
            return;
        }
        auto* co = static_cast<ObjCoroutine*>(obj);
        if (!co->profiler) {
            return;
        }
        m_heap.m_profilerData->mergeFrom(*co->profiler);
        co->profiler->clearStats();
    });
}
#endif
