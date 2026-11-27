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
        heapOf(obj)->releaseHold(obj);
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

void MemoryManager::releaseHold(const Obj* obj) {
    bool freeNow = false;
    {
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
        freeNow = !m_ownerAlive && m_heldTotal == 0;
    }
    if (freeNow) {
        delete this;
    }
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
        Obj* obj = as<Obj*>(v);
        heapOf(obj)->releaseHold(obj);
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
                                              bool dropStrings) {
    return m_foreign.scanStep(maxEntries, [this, dropStrings](Obj* obj) {
        // Before the release: the owner can free a String as soon as its
        // count drops, and the table must not hold it by then.
        if (dropStrings && obj->type == ObjType::STRING) {
            m_strings.del(static_cast<ObjString*>(obj));
        }
        heapOf(obj)->releaseHold(obj);
    });
}

void MemoryManager::releaseUnreachedForeign(bool dropStrings) {
    beginForeignRelease();
    while (foreignReleasing()) {
        releaseForeignStep(std::numeric_limits<std::size_t>::max(),
                           dropStrings);
    }
}

void MemoryManager::adoptForeign(Obj* obj) {
    if (!m_foreign.touch(obj)) {
        heapOf(obj)->releaseHold(obj);
    }
}

Value MemoryManager::receiveMessage(Value msg) {
    if (!isObj(msg)) {
        return msg;
    }
    Obj* obj = as<Obj*>(msg);
    if (obj->heapId == m_heapId) {
        releaseHold(obj);
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
            heapOf(str)->releaseHold(str);
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
