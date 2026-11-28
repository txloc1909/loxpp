#include "memory_manager.h"

#include <limits>
#include "compiler.h"
#include "objects.h"
#include "table.h"
#include "value.h"

#ifdef LOXPP_PROFILE
#include "profiler.h"
#include <optional>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

// A non-empty LOXPP_STRESS_GC value turns on collect-on-every-allocation for
// the lifetime of this heap, whatever the collector is. Read once here; never
// on an allocation path.
static bool readStressGCEnv() {
    const char* v = std::getenv("LOXPP_STRESS_GC");
    return v != nullptr && v[0] != '\0';
}

// The limit of a `stall` record, in nanoseconds. LOXPP_GC_STALL_LIMIT_US
// changes it; 0 records every call that reaches the C allocator.
static std::uint64_t stallLimitNs() {
    const char* v = std::getenv("LOXPP_GC_STALL_LIMIT_US");
    if (v == nullptr || v[0] == '\0') {
        return SizeClassHeap::kDefaultStallLimitNs;
    }
    return std::strtoull(v, nullptr, 10) * 1000;
}

MemoryManager::MemoryManager(int workerNumber)
    : m_strings(VmAllocator<Entry>{this}), m_workerNumber(workerNumber),
      m_heapId(acquireHeapId(this)) {
    m_inbox.workerNumber = workerNumber;
    m_poll.stress = readStressGCEnv();
    m_collector = makeCollector(*this, m_poll, workerNumber);
    m_stw = dynamic_cast<StwCollector*>(m_collector.get());
    VmAllocBase::barrierCollector = m_collector.get();
    m_store.setStallTrace(m_collector->traceFile(), stallLimitNs());
}

MemoryManager::~MemoryManager() {
    collectAll();
    // A dead heap keeps the foreign objects its held messages reach until it
    // is freed; a heap that never retired keeps every foreign object it
    // held.
    std::vector<Obj*> foreign;
    foreign.reserve(m_foreign.size());
    m_foreign.forEach([&foreign](Obj* obj) { foreign.push_back(obj); });
    m_foreign.clear();
    for (Obj* obj : foreign) {
        releaseCount(obj);
    }
    std::vector<Envelope> rest;
    {
        std::lock_guard<std::mutex> lock(m_inbox.mutex);
        rest.swap(m_inbox.queue);
    }
    for (const Envelope& env : rest) {
        if (isObj(env.msg) && as<Obj*>(env.msg)->heapId != m_heapId) {
            releaseValue(env.msg);
        }
    }
    // The counts that this destructor dropped can be the last ones of other
    // dead heaps. The thread of this heap ends, so it destroys them now: no
    // other thread may touch them.
    drainPendingDeadHeaps();
    // The collector closes the trace file.
    m_store.setStallTrace(nullptr, 0);
    m_stw = nullptr;
    VmAllocBase::barrierCollector = nullptr;
    m_collector.reset();
    releaseHeapId(m_heapId);
}

void* MemoryManager::rawAlloc(std::size_t bytes) {
    bytesAllocated += bytes;
    if (shouldPoll()) {
        m_collector->poll();
        reapPendingPart();
    }
    return m_store.allocBuffer(bytes);
}

void MemoryManager::rawFree(void* p, std::size_t bytes) noexcept {
    bytesAllocated -= bytes;
    m_store.freeBuffer(p, bytes);
}

void MemoryManager::release(Obj* obj, std::size_t size) {
    bytesAllocated -= size;
    obj->~Obj();
    m_store.freeObject(obj);
}

ObjString* MemoryManager::makeString(std::string_view sv) {
    uint32_t hash = hashString(sv);
    auto* interned =
        m_strings.findString(sv.data(), static_cast<int>(sv.size()), hash);
    if (interned != nullptr) {
        noteInternHit(interned);
        return interned;
    }

    auto* s = create<ObjString>(sv, hash, VmAllocator<char>{this});
    pushTempRoot(s); // protect s across m_strings.set's potential rawAlloc → GC
    m_strings.set(s, Value{Nil{}});
    popTempRoot();
    return s;
}

// A std::string has another allocator than LoxString, so its bytes are
// copied once; a caller that can build a LoxString saves that copy.
ObjString* MemoryManager::makeString(std::string&& sv) {
    return makeString(std::string_view{sv});
}

ObjString* MemoryManager::makeString(LoxString&& sv) {
    uint32_t hash = hashString(sv);
    auto* interned =
        m_strings.findString(sv.data(), static_cast<int>(sv.size()), hash);
    if (interned != nullptr) {
        noteInternHit(interned);
        return interned;
    }

    auto* s = create<ObjString>(std::move(sv), hash);
    pushTempRoot(s); // protect s across m_strings.set's potential rawAlloc → GC
    m_strings.set(s, Value{Nil{}});
    popTempRoot();
    return s;
}

ObjString* MemoryManager::findString(std::string_view sv) const {
    uint32_t hash = hashString(sv);
    ObjString* found =
        m_strings.findString(sv.data(), static_cast<int>(sv.size()), hash);
    if (found != nullptr) {
        noteInternHit(found);
    }
    return found;
}

std::unique_ptr<Collector>
MemoryManager::swapCollector(std::unique_ptr<Collector> collector) {
    std::swap(m_collector, collector);
    m_stw = dynamic_cast<StwCollector*>(m_collector.get());
    VmAllocBase::barrierCollector = m_collector.get();
    m_store.setStallTrace(m_collector->traceFile(), stallLimitNs());
    return collector;
}

void MemoryManager::pushTempRoot(Obj* obj) { m_tempRoots.push_back(obj); }
void MemoryManager::popTempRoot() { m_tempRoots.pop_back(); }

void MemoryManager::setMarkRootsCallback(std::function<void()> cb) {
    m_markRoots = std::move(cb);
}

void MemoryManager::setCurrentCompiler(Compiler* c) { m_currentCompiler = c; }

void MemoryManager::markFinishRoots() {
    markHeldRoots();
    markInboxTargets();
    for (const Envelope& env : m_draining) {
        markObject(env.target);
        markValue(env.msg);
    }
}

void MemoryManager::markAllRoots() {
    if (m_markRoots) {
        m_markRoots();
    }
    if (m_currentCompiler) {
        m_currentCompiler->markRoots(*this);
    }
    for (auto* obj : m_tempRoots) {
        markObject(obj);
    }
    markHeldRoots();
    markInboxTargets();
    for (const Envelope& env : m_draining) {
        markObject(env.target);
        markValue(env.msg);
    }
}

void MemoryManager::beginForeignCycle() { m_foreign.beginCycle(); }

void MemoryManager::markForeign(Obj* obj) {
    if (m_foreign.touch(obj)) {
        // The count moves when the mark reaches the object, not at the end of
        // the mark. The object is held now (the way to it is a count of an
        // envelope in flight or of this set), but this thread can give that
        // count back before the mark ends: a drain that receives the same
        // object releases the count of the envelope. Without the count of
        // this set, the owner could free the object in between.
        heapOf(obj)->hold(obj);
    }
}

bool MemoryManager::foreignMarked(const Obj* obj) const {
    return m_foreign.marked(obj);
}

#ifdef LOXPP_PROFILE
void MemoryManager::mergeCoroutineProfilers() {
    m_collector->mergeCoroutineProfilers();
}
#endif

// --- Workers --------------------------------------------------------------

void MemoryManager::hold(const Obj* obj) {
    std::lock_guard<std::mutex> lock(m_holdMutex);
    ++m_holders[obj];
    ++m_heldTotal;
}

bool MemoryManager::releaseHold(const Obj* obj) {
    std::lock_guard<std::mutex> lock(m_holdMutex);
    auto it = m_holders.find(obj);
    if (it == m_holders.end()) {
        std::fprintf(stderr, "BUG: a release of an object with no hold.\n");
        std::abort();
    }
    if (--it->second == 0) {
        m_holders.erase(it);
    }
    --m_heldTotal;
    return !m_ownerAlive && m_heldTotal == 0;
}

std::uint32_t MemoryManager::holderCount(const Obj* obj) {
    std::lock_guard<std::mutex> lock(m_holdMutex);
    auto it = m_holders.find(obj);
    return it == m_holders.end() ? 0 : it->second;
}

void MemoryManager::holdValue(const Value& v) {
    if (isObj(v) && as<Obj*>(v)->heapId != 0) {
        Obj* obj = as<Obj*>(v);
        heapOf(obj)->hold(obj);
    }
}

void MemoryManager::releaseValue(const Value& v) {
    if (isObj(v) && as<Obj*>(v)->heapId != 0) {
        releaseCount(as<Obj*>(v));
    }
}

void MemoryManager::releaseCount(Obj* obj) {
    MemoryManager* owner = heapOf(obj);
    if (owner->releaseHold(obj)) {
        m_destroying.push_back(owner);
    }
}

void MemoryManager::markHeldRoots() {
    std::lock_guard<std::mutex> lock(m_holdMutex);
    for (const auto& entry : m_holders) {
        markObject(const_cast<Obj*>(entry.first));
    }
}

void MemoryManager::markInboxTargets() {
    // A target stays alive while an envelope names it, also after its sender
    // released its own hold on the handle. The held roots were read first:
    // a release that this collection did not see in them came after the
    // envelope was appended, so the envelope is here.
    std::lock_guard<std::mutex> lock(m_inbox.mutex);
    for (const Envelope& env : m_inbox.queue) {
        markObject(env.target);
    }
}

void MemoryManager::beginForeignRelease() { m_foreign.beginScan(); }

void MemoryManager::endForeignRelease() { m_foreign.endScan(); }

bool MemoryManager::foreignReleasing() const { return m_foreign.scanning(); }

std::size_t MemoryManager::releaseForeignStep(std::size_t maxEntries,
                                              bool dropStrings,
                                              MemoryManager* reaper) {
    return m_foreign.scanStep(
        maxEntries, [this, dropStrings, reaper](Obj* obj) {
            // Before the release: the owner can free a String as soon as its
            // count drops, and the table must not hold it by then.
            if (dropStrings && obj->type == ObjType::STRING) {
                m_strings.del(static_cast<ObjString*>(obj));
            }
            reaper->releaseCount(obj);
        });
}

void MemoryManager::releaseUnreachedForeign(bool dropStrings) {
    beginForeignRelease();
    while (foreignReleasing()) {
        releaseForeignStep(std::numeric_limits<std::size_t>::max(), dropStrings,
                           this);
    }
}

void MemoryManager::adoptForeign(Obj* obj) {
    if (!m_foreign.touch(obj)) {
        releaseCount(obj);
    }
}

Value MemoryManager::receiveMessage(Value msg) {
    if (!isObj(msg)) {
        return msg;
    }
    Obj* obj = as<Obj*>(msg);
    if (obj->heapId == m_heapId) {
        releaseCount(obj);
        return msg;
    }
    if (obj->type == ObjType::STRING) {
        auto* str = static_cast<ObjString*>(obj);
        ObjString* found = m_strings.findString(
            str->chars.data(), static_cast<int>(str->chars.size()), str->hash);
        if (found == str) {
            noteInternHit(found);
            adoptForeign(str);
            return msg;
        }
        if (found != nullptr) {
            noteInternHit(found);
            releaseCount(str);
            return Value{static_cast<Obj*>(found)};
        }
        adoptForeign(str);
        pushTempRoot(str); // the table can grow, and a collection can run
        m_strings.set(str, Value{Nil{}});
        popTempRoot();
        return msg;
    }
    adoptForeign(obj);
    return msg;
}

void MemoryManager::retire() {
    std::vector<Envelope> rest;
    {
        std::lock_guard<std::mutex> lock(m_inbox.mutex);
        m_inbox.exited = true;
        m_inbox.runtime = nullptr;
        rest.swap(m_inbox.queue);
        m_inbox.nonEmpty.store(false);
    }
    rest.insert(rest.end(), m_draining.begin(), m_draining.end());
    m_draining.clear();
    for (const Envelope& env : rest) {
        releaseValue(env.msg);
    }
    m_markRoots = nullptr;
    m_currentCompiler = nullptr;
    m_tempRoots.clear();
#ifdef LOXPP_PROFILE
    m_profilerData = nullptr;
#endif
    m_collector->collectNow(CollectCause::Exit);
}

void MemoryManager::dropOwner() {
    // The thread of this heap ends, so the dead heaps that it took are
    // destroyed now: no other thread may touch them. The drain comes before
    // m_ownerAlive is cleared: a part of a drained heap can drop the last
    // count of this heap, and that would make this heap queue itself on
    // itself after the clear.
    drainPendingDeadHeaps();
    bool freeNow = false;
    {
        std::lock_guard<std::mutex> lock(m_holdMutex);
        m_ownerAlive = false;
        freeNow = m_heldTotal == 0;
    }
    if (freeNow) {
        delete this;
    }
}

std::size_t MemoryManager::reapPendingPart() {
    if (m_destroyingNow == nullptr) {
        if (m_destroying.empty()) {
            return 0;
        }
        m_destroyingNow = m_destroying.back();
        m_destroying.pop_back();
    }
    bool finished = false;
    // A part with no units would never finish, so the smallest part is one
    // unit.
    const std::size_t units = m_destroyUnits != 0 ? m_destroyUnits : 1;
    const std::size_t done =
        m_destroyingNow->destroyStep(*this, units, finished);
    if (finished) {
        MemoryManager* heap = m_destroyingNow;
        m_destroyingNow = nullptr;
        delete heap;
    }
    return done;
}

void MemoryManager::reapPendingIdle() {
    const std::uint64_t end = gcMonotonicNs() + kDestroyIdleNs;
    while (pendingDeadHeaps() != 0 && gcMonotonicNs() < end) {
        reapPendingPart();
    }
}

void MemoryManager::drainPendingDeadHeaps() {
    while (m_destroyingNow != nullptr || !m_destroying.empty()) {
        reapPendingPart();
    }
}

std::size_t MemoryManager::destroyStep(MemoryManager& reaper, std::size_t units,
                                       bool& finished) {
    finished = false;
    std::size_t done = 0;
    if (m_destroyStage == 0) {
        done = m_store.destroyObjectsStep(m_destroySpan, units);
        if (m_destroySpan != nullptr) {
            return done;
        }
        m_destroyStage = 1;
    }
    if (m_destroyStage == 1) {
        if (!m_destroyForeignStarted) {
            // Every entry is unmarked now, so the scan drops each one. A
            // mark of the last cycle may have touched it.
            m_foreign.beginCycle();
            beginForeignRelease();
            m_destroyForeignStarted = true;
        }
        while (foreignReleasing() && done < units) {
            done += releaseForeignStep(units - done, false, &reaper);
        }
        if (foreignReleasing()) {
            return done;
        }
        m_destroyStage = 2;
    }
    if (!m_destroyInboxTaken) {
        std::lock_guard<std::mutex> lock(m_inbox.mutex);
        m_destroyEnvelopes.swap(m_inbox.queue);
        m_inbox.nonEmpty.store(false);
        m_destroyInboxTaken = true;
    }
    while (!m_destroyEnvelopes.empty() && done < units) {
        Envelope env = m_destroyEnvelopes.back();
        m_destroyEnvelopes.pop_back();
        ++done;
        if (isObj(env.msg) && as<Obj*>(env.msg)->heapId != m_heapId) {
            reaper.releaseValue(env.msg);
        }
    }
    finished = m_destroyEnvelopes.empty();
    return done;
}
