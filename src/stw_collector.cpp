#include "stw_collector.h"
#include "memory_manager.h"
#include "object_trace.h"
#include "table.h"

#ifdef LOXPP_PROFILE
#include "profiler.h"
#include <optional>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#ifdef LOXPP_DEBUG_LOG_GC
static const char* objTypeName(ObjType type) {
    switch (type) {
    case ObjType::STRING:
        return "string";
    case ObjType::FUNCTION:
        return "function";
    case ObjType::NATIVE:
        return "native";
    case ObjType::UPVALUE:
        return "upvalue";
    case ObjType::CLOSURE:
        return "closure";
    case ObjType::CLASS:
        return "class";
    case ObjType::INSTANCE:
        return "instance";
    case ObjType::BOUND_METHOD:
        return "bound_method";
    case ObjType::LIST:
        return "list";
    case ObjType::FILE:
        return "file";
    case ObjType::ITERATOR:
        return "iterator";
    case ObjType::MAP:
        return "map";
    case ObjType::ENUM_CTOR:
        return "enum_ctor";
    case ObjType::ENUM:
        return "enum";
    case ObjType::BOUND_NATIVE:
        return "bound_native";
    case ObjType::ERROR:
        return "error";
    case ObjType::DEFERRED_CALL:
        return "deferred_call";
    case ObjType::SOCKET:
        return "socket";
    case ObjType::SERVER:
        return "server";
    case ObjType::PROCESS:
        return "process";
    case ObjType::COROUTINE:
        return "coroutine";
    case ObjType::TASK:
        return "task";
    case ObjType::INT:
        return "int";
    }
    return "?";
}
#endif

static uint64_t monotonicNs() { return gcMonotonicNs(); }

static std::FILE* openTraceFile(int workerNumber) {
    return openGcTraceFile(workerNumber);
}

StwCollector::StwCollector(MemoryManager& heap, PollState& poll,
                           int workerNumber)
    : Collector(poll), m_heap(heap), m_store(heap.store()),
      m_trace(openTraceFile(workerNumber)) {
    m_poll.pollAt = kMinHeap;
}

StwCollector::~StwCollector() {
    if (m_trace != nullptr) {
        std::fprintf(m_trace, "end t=%llu\n",
                     static_cast<unsigned long long>(monotonicNs()));
        std::fclose(m_trace);
    }
}

void StwCollector::releaseAll() {
    m_store.destroyAllObjects();
    m_heap.bytesAllocated = 0;
}

void StwCollector::markObject(Obj* obj) {
    if (obj == nullptr) {
        return;
    }
    if (obj->heapId != m_heap.heapId()) {
        // An object of another heap: the heap marks it in its side table
        // only, and does not trace it. Its owner traces it while this heap
        // holds it.
        if (obj->heapId == 0) {
            return;
        }
        m_heap.markForeign(obj);
        return;
    }
    if (!m_store.tryMark(obj)) {
        return;
    }
    m_grayStack.push_back(obj);
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] mark   %p %s\n", static_cast<void*>(obj),
            stringifyObj(obj).c_str());
#endif
}

void StwCollector::traceReferences() {
    while (!m_grayStack.empty()) {
        Obj* obj = m_grayStack.back();
        m_grayStack.pop_back();
        traceObjectEdges(obj, *this);
    }
}

bool StwCollector::isMarkedHere(const Obj* obj) const {
    if (obj->heapId == m_heap.heapId()) {
        return m_store.isMarked(obj);
    }
    return m_heap.foreignMarked(obj);
}

void StwCollector::removeWhiteStrings() {
    m_heap.m_strings.removeKeysIf(
        [this](const ObjString* key) { return !isMarkedHere(key); });
}

std::size_t StwCollector::sweep() {
#ifdef LOXPP_PROFILE
    // An abandoned coroutine is only reachable through its snapshot; sweep is
    // the last chance to fold its profile into the report. The merge can
    // allocate and throw, so it runs before any object is freed.
    if (m_heap.m_profilerData) {
        m_store.forEachObject([this](Obj* obj, bool marked) {
            if (marked || obj->type != ObjType::COROUTINE)
                return;
            auto* co = static_cast<ObjCoroutine*>(obj);
            if (co->profiler)
                m_heap.m_profilerData->mergeFrom(*co->profiler);
        });
    }
#endif
    std::size_t survivors = 0;
    for (SizeClassHeap::Span* s = m_store.firstObjectSpan(); s != nullptr;
         s = SizeClassHeap::nextObjectSpan(s)) {
        survivors += m_store.sweepSpan(s, [this](Obj* obj) {
#ifdef LOXPP_DEBUG_LOG_GC
            // Use type-only log: stringifyObj dereferences fn->name which may
            // already be freed if the name ObjString appeared earlier.
            fprintf(stderr, "[GC] free   %p (%s)\n", static_cast<void*>(obj),
                    objTypeName(obj->type));
#endif
            m_heap.bytesAllocated -= objAllocatedSize(obj);
        });
        // A freed large block goes back to the kernel one slice for each span.
        m_store.releaseStep();
    }
    m_poll.pollAt =
        std::max(m_heap.bytesAllocated * GC_HEAP_GROW_FACTOR, kMinHeap);
    return survivors;
}

#ifdef LOXPP_PROFILE
void StwCollector::mergeCoroutineProfilers() {
    if (!m_heap.m_profilerData)
        return;
    m_store.forEachObject([this](Obj* obj, bool) {
        if (obj->type != ObjType::COROUTINE)
            return;
        auto* co = static_cast<ObjCoroutine*>(obj);
        if (!co->profiler)
            return;
        m_heap.m_profilerData->mergeFrom(*co->profiler);
        co->profiler->clearStats();
    });
}
#endif

const char* StwCollector::collectionCause() const {
    if (m_exitCollection) {
        return "exit";
    }
    return m_poll.stress ? "stress" : "threshold";
}

void StwCollector::collectNow(CollectCause cause) {
    m_exitCollection = cause == CollectCause::Exit;
    // The traced instantiation holds every clock read and counter, so the
    // untraced one carries none of them.
    if (m_trace != nullptr) {
        runCollection<true>();
    } else {
        runCollection<false>();
    }
    m_exitCollection = false;
    ++m_collectionCount;
}

template <bool Traced>
void StwCollector::runCollection() {
#ifdef LOXPP_PROFILE
    // ProfileGcScope destructor fires when runCollection() returns.
    // It reads bytesAllocated by const-ref; by then sweep has updated it.
    std::optional<ProfileGcScope> gcScope;
    if (m_heap.m_profilerData)
        gcScope.emplace(*m_heap.m_profilerData, m_heap.bytesAllocated);
#endif
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] begin -- %zu bytes allocated\n",
            m_heap.bytesAllocated);
#endif
    uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    std::size_t bytesBefore = 0, objsBefore = 0;
    m_heap.beginForeignCycle();
    if constexpr (Traced) {
        bytesBefore = m_heap.bytesAllocated;
        objsBefore = m_store.objectCount();
        t0 = monotonicNs();
    }
    m_heap.markAllRoots();
    if constexpr (Traced) {
        t1 = monotonicNs();
    }
    traceReferences();
    if constexpr (Traced) {
        t2 = monotonicNs();
    }
    removeWhiteStrings();
    if constexpr (Traced) {
        t3 = monotonicNs();
    }
    std::size_t marked = sweep();
    m_heap.releaseUnreachedForeign();
    if constexpr (Traced) {
        t4 = monotonicNs();
        // Flushed per line so a process that stops without stdio cleanup (a
        // signal, abort(), _exit()) still leaves every collection so far.
        std::fprintf(m_trace,
                     "gc cause=%s t0=%llu t1=%llu t2=%llu t3=%llu t4=%llu "
                     "bytes_before=%zu bytes_after=%zu objs_before=%zu "
                     "objs_after=%zu marked=%zu freed=%zu\n",
                     collectionCause(), static_cast<unsigned long long>(t0),
                     static_cast<unsigned long long>(t1),
                     static_cast<unsigned long long>(t2),
                     static_cast<unsigned long long>(t3),
                     static_cast<unsigned long long>(t4), bytesBefore,
                     m_heap.bytesAllocated, objsBefore, marked, marked,
                     objsBefore - marked);
        std::fflush(m_trace);
    }
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] end   -- %zu bytes allocated, next threshold %zu\n",
            m_heap.bytesAllocated, m_poll.pollAt);
#endif
}