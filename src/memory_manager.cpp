#include "memory_manager.h"
#include "compiler.h"
#include "objects.h"
#include "table.h"
#include "value.h"

#ifdef LOXPP_PROFILE
#include "profiler.h"
#include <optional>
#endif

#include <cstdio>
#include <cstdlib>
#include <ctime>

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
    }
    return "?";
}
#endif

// A non-empty LOXPP_STRESS_GC value turns on collect-on-every-allocation for
// the lifetime of this MemoryManager. Read once here; never on an allocation
// path.
static bool readStressGCEnv() {
    const char* v = std::getenv("LOXPP_STRESS_GC");
    return v != nullptr && v[0] != '\0';
}

static uint64_t monotonicNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Trace line formats are documented in tools/gc_report.py.
static std::FILE* openTraceFile() {
    const char* path = std::getenv("LOXPP_GC_TRACE");
    if (path == nullptr || path[0] == '\0') {
        return nullptr;
    }
    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) {
        std::fprintf(stderr, "LOXPP_GC_TRACE: cannot open '%s'\n", path);
        return nullptr;
    }
    std::fprintf(f, "# loxpp-gc-trace v1\nstart t=%llu\n",
                 static_cast<unsigned long long>(monotonicNs()));
    std::fflush(f);
    return f;
}

MemoryManager::MemoryManager()
    : m_strings(VmAllocator<Entry>{this}), m_stressGC(readStressGCEnv()),
      m_trace(openTraceFile()) {}

MemoryManager::~MemoryManager() {
    collectAll();
    if (m_trace != nullptr) {
        std::fprintf(m_trace, "end t=%llu\n",
                     static_cast<unsigned long long>(monotonicNs()));
        std::fclose(m_trace);
    }
}

void* MemoryManager::rawAlloc(std::size_t bytes) {
    bytesAllocated += bytes;
    if (m_stressGC || bytesAllocated > m_nextGC) {
        collectGarbage();
    }
    return ::operator new(bytes);
}

void MemoryManager::release(Obj* obj, std::size_t size) {
    bytesAllocated -= size;
    delete obj;
}

ObjString* MemoryManager::makeString(std::string_view sv) {
    uint32_t hash = hashString(sv);
    auto* interned =
        m_strings.findString(sv.data(), static_cast<int>(sv.size()), hash);
    if (interned != nullptr) {
        return interned;
    }

    auto* s = create<ObjString>(sv, VmAllocator<char>{this});
    pushTempRoot(s); // protect s across m_strings.set's potential rawAlloc → GC
    m_strings.set(s, Value{Nil{}});
    popTempRoot();
    return s;
}

ObjString* MemoryManager::makeString(std::string&& sv) {
    uint32_t hash = hashString(sv);
    auto* interned =
        m_strings.findString(sv.data(), static_cast<int>(sv.size()), hash);
    if (interned != nullptr) {
        return interned;
    }

    auto* s = create<ObjString>(std::string_view{sv}, VmAllocator<char>{this});
    pushTempRoot(s); // protect s across m_strings.set's potential rawAlloc → GC
    m_strings.set(s, Value{Nil{}});
    popTempRoot();
    return s;
}

ObjString* MemoryManager::findString(std::string_view sv) const {
    uint32_t hash = hashString(sv);
    return m_strings.findString(sv.data(), static_cast<int>(sv.size()), hash);
}

void MemoryManager::collectAll() {
    for (Obj* o : allObjects) {
        delete o;
    }
    allObjects.clear();
    bytesAllocated = 0;
}

void MemoryManager::pushTempRoot(Obj* obj) { m_tempRoots.push_back(obj); }
void MemoryManager::popTempRoot() { m_tempRoots.pop_back(); }

void MemoryManager::setMarkRootsCallback(std::function<void()> cb) {
    m_markRoots = std::move(cb);
}

void MemoryManager::setCurrentCompiler(Compiler* c) { m_currentCompiler = c; }

void MemoryManager::markObject(Obj* obj) {
    if (obj == nullptr || obj->marked) {
        return;
    }
    obj->marked = true;
    m_grayStack.push_back(obj);
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] mark   %p %s\n", static_cast<void*>(obj),
            stringifyObj(obj).c_str());
#endif
}

void MemoryManager::markValue(const Value& v) {
    if (isObj(v)) {
        markObject(as<Obj*>(v));
    }
}

void MemoryManager::traceReferences() {
    while (!m_grayStack.empty()) {
        Obj* obj = m_grayStack.back();
        m_grayStack.pop_back();
        traceObject(obj);
    }
}

void MemoryManager::traceObject(Obj* obj) {
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] trace  %p %s\n", static_cast<void*>(obj),
            stringifyObj(obj).c_str());
#endif
    switch (obj->type) {
    case ObjType::STRING:
        break;
    case ObjType::NATIVE:
        break;
    case ObjType::UPVALUE:
        markValue(static_cast<ObjUpvalue*>(obj)->closed);
        markObject(static_cast<ObjUpvalue*>(obj)->owner);
        break;
    case ObjType::FUNCTION: {
        auto* fn = static_cast<ObjFunction*>(obj);
        markObject(fn->name);
        const auto& consts = fn->chunk.constants();
        for (uint16_t i = 0; i < consts.size(); i++) {
            markValue(consts.at(i));
        }
        break;
    }
    case ObjType::CLOSURE: {
        auto* cl = static_cast<ObjClosure*>(obj);
        markObject(cl->function);
        for (auto* uv : cl->upvalues) {
            markObject(uv);
        }
        break;
    }
    case ObjType::CLASS: {
        auto* klass = static_cast<ObjClass*>(obj);
        markObject(klass->name);
        if (klass->superclass) {
            markObject(klass->superclass);
        }
        klass->methods.forEach([this](ObjString* k, Value v) {
            markObject(k);
            markValue(v);
        });
        break;
    }
    case ObjType::INSTANCE: {
        auto* inst = static_cast<ObjInstance*>(obj);
        markObject(inst->klass);
        inst->fields.forEach([this](ObjString* k, Value v) {
            markObject(k);
            markValue(v);
        });
        break;
    }
    case ObjType::BOUND_METHOD: {
        auto* bm = static_cast<ObjBoundMethod*>(obj);
        markValue(bm->receiver);
        markObject(bm->method);
        break;
    }
    case ObjType::LIST: {
        auto* list = static_cast<ObjList*>(obj);
        for (auto& v : list->elements) {
            markValue(v);
        }
        break;
    }
    case ObjType::FILE:
        markObject(static_cast<ObjFile*>(obj)->klass);
        break;
    case ObjType::ITERATOR: {
        auto* it = static_cast<ObjIterator*>(obj);
        markValue(it->collection);
        if (it->hasCurrent) {
            markValue(it->current);
        }
        break;
    }
    case ObjType::MAP: {
        auto* map = static_cast<ObjMap*>(obj);
        markObject(map->klass);
        map->map.forEach([this](const MapEntry& e) {
            markValue(e.key);
            markValue(e.value);
        });
        break;
    }
    case ObjType::ENUM_CTOR: {
        auto* ctor = static_cast<ObjEnumCtor*>(obj);
        markObject(ctor->ctorName);
        markObject(ctor->enumName);
        break;
    }
    case ObjType::ENUM: {
        auto* e = static_cast<ObjEnum*>(obj);
        markObject(e->ctor);
        for (auto& v : e->fields) {
            markValue(v);
        }
        break;
    }
    case ObjType::BOUND_NATIVE: {
        auto* bn = static_cast<ObjBoundNative*>(obj);
        markValue(bn->receiver);
        markObject(bn->native);
        break;
    }
    case ObjType::ERROR: {
        auto* err = static_cast<ObjError*>(obj);
        markObject(err->klass);
        markObject(err->message);
        markObject(err->kind);
        break;
    }
    case ObjType::DEFERRED_CALL: {
        auto* deferred = static_cast<ObjDeferredCall*>(obj);
        markValue(deferred->callable);
        for (auto& v : deferred->args) {
            markValue(v);
        }
        break;
    }
    case ObjType::SOCKET:
        markObject(static_cast<ObjSocket*>(obj)->klass);
        break;
    case ObjType::SERVER:
        markObject(static_cast<ObjServer*>(obj)->klass);
        break;
    case ObjType::PROCESS:
        markObject(static_cast<ObjProcess*>(obj)->klass);
        break;
    case ObjType::COROUTINE: {
        auto* co = static_cast<ObjCoroutine*>(obj);
        markObject(co->klass);
        markValue(co->callee);
        for (const auto& v : co->stack) {
            markValue(v);
        }
        for (const auto& frame : co->frames) {
            markObject(frame.closure);
        }
        for (const auto& deferList : co->defers) {
            for (const auto& v : deferList) {
                markValue(v);
            }
        }
        for (ObjUpvalue* uv : co->openUpvalues) {
            markObject(uv);
        }
        for (const auto& v : co->resultOverrides) {
            markValue(v);
        }
        break;
    }
    }
}

void MemoryManager::removeWhiteStrings() { m_strings.removeUnmarkedKeys(); }

// Returns the sizeof(T) used when the object was created via create<T>().
// Must match the sizeof(T) added in create<T>().
static std::size_t objAllocatedSize(Obj* obj) {
    switch (obj->type) {
    case ObjType::STRING:
        return sizeof(ObjString);
    case ObjType::FUNCTION:
        return sizeof(ObjFunction);
    case ObjType::NATIVE:
        return sizeof(ObjNative);
    case ObjType::UPVALUE:
        return sizeof(ObjUpvalue);
    case ObjType::CLOSURE:
        return sizeof(ObjClosure);
    case ObjType::CLASS:
        return sizeof(ObjClass);
    case ObjType::INSTANCE:
        return sizeof(ObjInstance);
    case ObjType::BOUND_METHOD:
        return sizeof(ObjBoundMethod);
    case ObjType::LIST:
        return sizeof(ObjList);
    case ObjType::FILE:
        return sizeof(ObjFile);
    case ObjType::ITERATOR:
        return sizeof(ObjIterator);
    case ObjType::MAP:
        return sizeof(ObjMap);
    case ObjType::ENUM_CTOR:
        return sizeof(ObjEnumCtor);
    case ObjType::ENUM:
        return sizeof(ObjEnum);
    case ObjType::BOUND_NATIVE:
        return sizeof(ObjBoundNative);
    case ObjType::ERROR:
        return sizeof(ObjError);
    case ObjType::DEFERRED_CALL:
        return sizeof(ObjDeferredCall);
    case ObjType::SOCKET:
        return sizeof(ObjSocket);
    case ObjType::SERVER:
        return sizeof(ObjServer);
    case ObjType::PROCESS:
        return sizeof(ObjProcess);
    case ObjType::COROUTINE:
        return sizeof(ObjCoroutine);
    }
    return 0;
}

void MemoryManager::sweep() {
#ifdef LOXPP_PROFILE
    // An abandoned coroutine is only reachable through its snapshot; sweep is
    // the last chance to fold its profile into the report. The merge can
    // allocate and throw, so it runs before the compaction changes anything.
    if (m_profilerData) {
        for (Obj* obj : allObjects) {
            if (obj->marked || obj->type != ObjType::COROUTINE)
                continue;
            auto* co = static_cast<ObjCoroutine*>(obj);
            if (co->profiler)
                m_profilerData->mergeFrom(*co->profiler);
        }
    }
#endif
    // Survivors are compacted in place with a write index: erasing each dead
    // object separately would shift the tail every time and make a sweep
    // quadratic in the heap size.
    std::size_t kept = 0;
    for (Obj* obj : allObjects) {
        if (obj->marked) {
            obj->marked = false;
            allObjects[kept++] = obj;
        } else {
#ifdef LOXPP_DEBUG_LOG_GC
            // Use type-only log: stringifyObj dereferences fn->name which may
            // already be freed if the name ObjString appeared earlier in
            // allObjects.
            fprintf(stderr, "[GC] free   %p (%s)\n", static_cast<void*>(obj),
                    objTypeName(obj->type));
#endif
            bytesAllocated -= objAllocatedSize(obj);
            delete obj;
        }
    }
    allObjects.resize(kept);
    m_nextGC = bytesAllocated * GC_HEAP_GROW_FACTOR;
}

#ifdef LOXPP_PROFILE
void MemoryManager::mergeCoroutineProfilers() {
    if (!m_profilerData)
        return;
    for (Obj* obj : allObjects) {
        if (obj->type != ObjType::COROUTINE)
            continue;
        auto* co = static_cast<ObjCoroutine*>(obj);
        if (!co->profiler)
            continue;
        m_profilerData->mergeFrom(*co->profiler);
        co->profiler->clearStats();
    }
}
#endif

void MemoryManager::collectGarbage() {
    // The traced instantiation holds every clock read and counter, so the
    // untraced one carries none of them.
    if (m_trace != nullptr) {
        runCollection<true>();
    } else {
        runCollection<false>();
    }
}

template <bool Traced>
void MemoryManager::runCollection() {
#ifdef LOXPP_PROFILE
    // ProfileGcScope destructor fires when runCollection() returns.
    // It reads bytesAllocated by const-ref; by then sweep has updated it.
    std::optional<ProfileGcScope> gcScope;
    if (m_profilerData)
        gcScope.emplace(*m_profilerData, bytesAllocated);
#endif
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] begin -- %zu bytes allocated\n", bytesAllocated);
#endif
    uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    std::size_t bytesBefore = 0, objsBefore = 0;
    if constexpr (Traced) {
        bytesBefore = bytesAllocated;
        objsBefore = allObjects.size();
        t0 = monotonicNs();
    }
    if (m_markRoots) {
        m_markRoots();
    }
    if (m_currentCompiler) {
        m_currentCompiler->markRoots(*this);
    }
    for (auto* obj : m_tempRoots) {
        markObject(obj);
    }
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
    sweep();
    if constexpr (Traced) {
        t4 = monotonicNs();
        // Survivors are exactly the objects that were marked.
        std::size_t marked = allObjects.size();
        // Flushed per line so a process that stops without stdio cleanup (a
        // signal, abort(), _exit()) still leaves every collection so far.
        std::fprintf(m_trace,
                     "gc cause=%s t0=%llu t1=%llu t2=%llu t3=%llu t4=%llu "
                     "bytes_before=%zu bytes_after=%zu objs_before=%zu "
                     "objs_after=%zu marked=%zu freed=%zu\n",
                     m_stressGC ? "stress" : "threshold",
                     static_cast<unsigned long long>(t0),
                     static_cast<unsigned long long>(t1),
                     static_cast<unsigned long long>(t2),
                     static_cast<unsigned long long>(t3),
                     static_cast<unsigned long long>(t4), bytesBefore,
                     bytesAllocated, objsBefore, marked, marked,
                     objsBefore - marked);
        std::fflush(m_trace);
    }
#ifdef LOXPP_DEBUG_LOG_GC
    fprintf(stderr, "[GC] end   -- %zu bytes allocated, next threshold %zu\n",
            bytesAllocated, m_nextGC);
#endif
}
