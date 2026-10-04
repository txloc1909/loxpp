#pragma once

#include "class_objects.h"
#include "core_hash_map.h"
#include "object.h"
#include "value.h"
#include "vm_allocator.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#ifdef LOXPP_PROFILE
#include "profiler.h"
#endif

struct ObjList : public Obj {
    VmVector<Value> elements;

    explicit ObjList(VmAllocator<Value> alloc)
        : Obj(ObjType::LIST), elements(alloc) {}
};

inline bool isObjList(Obj* o) { return isObjType(o, ObjType::LIST); }
inline ObjList* asObjList(Obj* o) { return static_cast<ObjList*>(o); }
inline bool isList(const Value& v) { return isValueOfType<ObjType::LIST>(v); }

struct ObjIterator : public Obj {
    // ObjList*, ObjString*, ObjMap*, or ObjCoroutine* being iterated.
    Value collection;
    int index; // current cursor position
    // Map structural version recorded at GET_ITER; -1 unless collection is a
    // Map. Any insert or erase during map iteration is an error, even when a
    // paired erase and insert restore the net size.
    int expectedVersion;
    // Coroutine mode only, and meaningful while hasCurrent is true: the value
    // ITER_HAS_NEXT resumed out of the coroutine, cached for ITER_NEXT to
    // push. The resume happens at has-next time because its outcome is what
    // decides whether another element exists at all.
    Value current;
    bool hasCurrent{false};

    ObjIterator(Value coll, int idx = 0, int expected = -1)
        : Obj(ObjType::ITERATOR), collection(coll), index(idx),
          expectedVersion(expected), current{Nil{}} {}
};

inline bool isObjIterator(Obj* o) { return isObjType(o, ObjType::ITERATOR); }
inline ObjIterator* asObjIterator(Obj* o) {
    return static_cast<ObjIterator*>(o);
}
inline bool isIterator(const Value& v) {
    return isValueOfType<ObjType::ITERATOR>(v);
}

struct ObjFile : public Obj {
    ObjClass* klass;       // shared s_fileClass; GC-tracked
    FILE* handle{nullptr}; // null when closed
    bool readable{false};
    bool writable{false};

    explicit ObjFile(ObjClass* k) : Obj(ObjType::FILE), klass(k) {}
    // Do NOT rely on this destructor to close file handles. GC timing is
    // non-deterministic; an open file may not be collected until program exit
    // (or never, if the heap is not exhausted). Use `defer f.close()` to
    // guarantee cleanup on function exit, or call f.close() explicitly.
    ~ObjFile() override {
        if (handle != nullptr) {
            std::fclose(handle);
            handle = nullptr;
        }
    }
};

inline bool isObjFile(Obj* o) { return isObjType(o, ObjType::FILE); }
inline ObjFile* asObjFile(Obj* o) { return static_cast<ObjFile*>(o); }
inline bool isFile(const Value& v) { return isValueOfType<ObjType::FILE>(v); }

// ---------------------------------------------------------------------------
// ObjSocket — a connected TCP stream (net_api.cpp)
// ---------------------------------------------------------------------------
// `handle` owns `fd`: fclose() closes it. `fd` is kept separately only so
// close_write() can shutdown(SHUT_WR) the half-close without tearing down the
// read direction. Do not rely on the destructor to close sockets; like
// ObjFile, GC timing is non-deterministic. Call close() explicitly, or use
// `defer s.close()`.
struct ObjSocket : public Obj {
    ObjClass* klass;       // shared Socket class; GC-tracked
    int fd{-1};            // raw descriptor; -1 when closed
    FILE* handle{nullptr}; // fdopen'd stream; null when closed
    bool writeClosed{false};

    explicit ObjSocket(ObjClass* k) : Obj(ObjType::SOCKET), klass(k) {}
    ~ObjSocket() override;
};

inline bool isObjSocket(Obj* o) { return isObjType(o, ObjType::SOCKET); }
inline ObjSocket* asObjSocket(Obj* o) { return static_cast<ObjSocket*>(o); }
inline bool isSocket(const Value& v) {
    return isValueOfType<ObjType::SOCKET>(v);
}

// ---------------------------------------------------------------------------
// ObjServer — a bound, listening TCP socket (net_api.cpp)
// ---------------------------------------------------------------------------
struct ObjServer : public Obj {
    ObjClass* klass; // shared Server class; GC-tracked
    int fd{-1};      // listening descriptor; -1 when closed
    int boundPort{0};

    explicit ObjServer(ObjClass* k) : Obj(ObjType::SERVER), klass(k) {}
    ~ObjServer() override;
};

inline bool isObjServer(Obj* o) { return isObjType(o, ObjType::SERVER); }
inline ObjServer* asObjServer(Obj* o) { return static_cast<ObjServer*>(o); }
inline bool isServer(const Value& v) {
    return isValueOfType<ObjType::SERVER>(v);
}

// ---------------------------------------------------------------------------
// ObjProcess — a child process with three pipe streams (process_api.cpp)
// ---------------------------------------------------------------------------
// `in` is the child's standard input (writable), `out` its standard output and
// `err` its standard error (readable). A child should be reaped with wait();
// the destructor only closes the pipes and reaps non-blockingly, so a child
// left running is not killed and a status not yet collected is lost.
struct ObjProcess : public Obj {
    ObjClass* klass; // shared Process class; GC-tracked
    long pid{-1};
    FILE* in{nullptr};  // child stdin (parent writes)
    FILE* out{nullptr}; // child stdout (parent reads)
    FILE* err{nullptr}; // child stderr (parent reads)
    bool reaped{false};
    int status{-1}; // valid once reaped

    explicit ObjProcess(ObjClass* k) : Obj(ObjType::PROCESS), klass(k) {}
    ~ObjProcess() override;
};

inline bool isObjProcess(Obj* o) { return isObjType(o, ObjType::PROCESS); }
inline ObjProcess* asObjProcess(Obj* o) { return static_cast<ObjProcess*>(o); }
inline bool isProcess(const Value& v) {
    return isValueOfType<ObjType::PROCESS>(v);
}

// ---------------------------------------------------------------------------
// ObjError — error value caught by try/catch
// ---------------------------------------------------------------------------
struct ObjError : public Obj {
    ObjClass* klass;    // shared s_errorClass; GC-tracked
    ObjString* message; // human-readable error message
    ObjString* kind;    // error category string (e.g. "ArithmeticTypeError")

    ObjError(ObjClass* k, ObjString* msg, ObjString* k_str)
        : Obj(ObjType::ERROR), klass(k), message(msg), kind(k_str) {}
};

inline bool isObjError(Obj* o) { return isObjType(o, ObjType::ERROR); }
inline ObjError* asObjError(Obj* o) { return static_cast<ObjError*>(o); }
inline bool isError(const Value& v) { return isValueOfType<ObjType::ERROR>(v); }

// ---------------------------------------------------------------------------
// ObjMap — open-addressing hash map with Value keys and values.
// Keys must be Bool, Number, Nil, or String (interned).
// ---------------------------------------------------------------------------

enum class MapSlot : uint8_t { EMPTY, OCCUPIED, TOMBSTONE };

struct MapEntry {
    Value key{Nil{}};
    Value value{Nil{}};
    // The key's hash, computed once when the entry is inserted. CoreHashMap's
    // grow() rehashes every live entry from this field, so it must never call
    // back into the VM (a user __hash__ needs a running interpreter).
    uint32_t hash{0};
    MapSlot state{MapSlot::EMPTY};
};

struct MapPolicy {
    static bool isEmpty(const MapEntry& e) { return e.state == MapSlot::EMPTY; }
    static bool isTombstone(const MapEntry& e) {
        return e.state == MapSlot::TOMBSTONE;
    }
    static void makeTombstone(MapEntry& e) {
        e.key = Value{Nil{}};
        e.value = Value{Nil{}};
        e.state = MapSlot::TOMBSTONE;
    }
    static uint32_t
    hashOf(const MapEntry& e); // defined in container_objects.cpp
    // Identity match, used by grow() and the scalar/string-only insert path.
    // The VM path passes its own equality callback to CoreHashMap instead.
    static bool keyMatch(const MapEntry& slot, const MapEntry& needle) {
        return slot.key == needle.key;
    }
};

struct ObjMap : public Obj {
    ObjClass* klass; // shared s_mapClass, for method dispatch
    CoreHashMap<MapEntry, MapPolicy, VmAllocator<MapEntry>> map;
    // Structural version, bumped by mapSet on a new key and by mapDel on a
    // real erase only. Overwrites and misses leave it alone. Plain int packs
    // with CoreHashMap's own counts; wrap needs 2B changes in one loop.
    int version{0};
    // Non-zero while a key operation (hash + probe) runs on this map. A
    // mapSet/mapDel then raises MapChangedError, so a user __hash__/__eq__
    // cannot reallocate the bucket array under an in-progress probe.
    int keyOpDepth{0};

    // Key equality callback for the VM path: (storedKey, lookupKey) -> equal.
    // It may dispatch a user __eq__, so it can fail; the Runtime wrapper that
    // supplies it records the failure and checks it after the map call.
    using KeyEq = std::function<bool(const Value&, const Value&)>;

    ObjMap(ObjClass* k, VmAllocator<MapEntry> alloc)
        : Obj(ObjType::MAP), klass(k), map(alloc) {}

    // Scalar/string path: identity equality, hash computed with hashValue().
    // Returns true if key was newly inserted (false = update).
    bool mapSet(const Value& key, const Value& value);
    bool mapGet(const Value& key, Value& out) const;
    // Returns true if key was found and removed.
    bool mapDel(const Value& key);

    // VM path: the caller supplies the precomputed hash and the equality
    // callback, so an Instance key can dispatch __hash__/__eq__. `eqFailed` is
    // set true by the callback when a user __eq__ produced an error; a set or
    // del then leaves the map unchanged, so a failed key comparison cannot
    // mutate the map.
    bool mapSetHashed(const Value& key, const Value& value, uint32_t hash,
                      const KeyEq& eq, bool& eqFailed);
    bool mapGetHashed(const Value& key, uint32_t hash, const KeyEq& eq,
                      Value& out) const;
    bool mapDelHashed(const Value& key, uint32_t hash, const KeyEq& eq,
                      bool& eqFailed);
};

inline bool isObjMap(Obj* o) { return isObjType(o, ObjType::MAP); }
inline ObjMap* asObjMap(Obj* o) { return static_cast<ObjMap*>(o); }
inline bool isMap(const Value& v) { return isValueOfType<ObjType::MAP>(v); }

// ---------------------------------------------------------------------------
// ObjEnumCtor — callable constructor created by an enum declaration.
// ---------------------------------------------------------------------------
struct ObjEnumCtor : public Obj {
    uint8_t tag;
    uint8_t arity;
    ObjString* ctorName;
    ObjString* enumName;

    ObjEnumCtor(uint8_t t, uint8_t a, ObjString* cn, ObjString* en)
        : Obj(ObjType::ENUM_CTOR), tag(t), arity(a), ctorName(cn),
          enumName(en) {}
};

inline bool isEnumCtor(const Value& v) {
    return isValueOfType<ObjType::ENUM_CTOR>(v);
}
inline ObjEnumCtor* asObjEnumCtor(Obj* o) {
    return static_cast<ObjEnumCtor*>(o);
}

// ---------------------------------------------------------------------------
// ObjEnum — value produced by calling an ObjEnumCtor.
// ---------------------------------------------------------------------------
struct ObjEnum : public Obj {
    ObjEnumCtor* ctor;
    VmVector<Value> fields;

    ObjEnum(ObjEnumCtor* c, VmAllocator<Value> alloc)
        : Obj(ObjType::ENUM), ctor(c), fields(alloc) {}
};

inline bool isEnumValue(const Value& v) {
    return isValueOfType<ObjType::ENUM>(v);
}
inline ObjEnum* asObjEnum(Obj* o) { return static_cast<ObjEnum*>(o); }

// ---------------------------------------------------------------------------
// ObjDeferredCall — captured callable and its arguments, pending invocation
// ---------------------------------------------------------------------------
struct ObjDeferredCall : public Obj {
    Value callable;       // ObjClosure* - the function to call
    VmVector<Value> args; // arguments to pass

    ObjDeferredCall(Value fn, VmAllocator<Value> alloc)
        : Obj(ObjType::DEFERRED_CALL), callable(fn), args(alloc) {}
};

inline bool isObjDeferredCall(Obj* o) {
    return isObjType(o, ObjType::DEFERRED_CALL);
}
inline ObjDeferredCall* asObjDeferredCall(Obj* o) {
    return static_cast<ObjDeferredCall*>(o);
}
inline bool isDeferredCall(const Value& v) {
    return isValueOfType<ObjType::DEFERRED_CALL>(v);
}

// ---------------------------------------------------------------------------
// ObjCoroutine — a suspendable computation (spec/03-types.md, §Coroutine).
// ---------------------------------------------------------------------------
// A coroutine owns a frozen interpreter snapshot (copy-on-suspend): the
// operand-stack slice, call frames, handler records, defer entries, open
// upvalues, and result-check slots whose frames lie inside that slice. While
// the coroutine runs, its state is live on the shared VM stack; a `yield`
// copies it into these vectors and truncates the shared stack back, and a
// `resume` copies it back out and rebases every pointer. A running coroutine
// has an empty snapshot; a suspended one has its whole state here.
//
// Every pointer that would point into the shared VM stack is stored as an
// offset from the coroutine's stack base, so a snapshot is pointer-independent
// until resume rebases it.
enum class CoroutineState : std::uint8_t {
    SUSPENDED, // not started, or yielded; resumable
    RUNNING,   // currently executing
    NORMAL,    // resumed another coroutine and awaits its yield/return
    DEAD,      // the function returned, or a throw left the coroutine
};

struct CoroutineFrameSnapshot {
    ObjClosure* closure;
    int ipOffset;   // offset into closure->function->chunk
    int slotOffset; // offset into the coroutine's stack slice
    // Coroutine mode (QBE #530/#535): the compiled frame's own resume point,
    // restored into CallFrame::compiledState. 0 for an interpreted frame.
    int compiledState{0};
};

struct CoroutineHandlerSnapshot {
    int frameOffset; // frame index relative to the coroutine's base frame
    int stackOffset; // operand-stack index relative to the stack base
    Chunk::const_iterator catchIp;
    // Coroutine mode: the compiled frame's catch resume point, restored into
    // HandlerRecord::catchState. -1 for an interpreted handler.
    int catchState{-1};
};

struct ObjCoroutine : public Obj {
    ObjClass* klass; // shared Coroutine class; GC-tracked
    Value callee;    // Function or BoundMethod to run
    CoroutineState state{CoroutineState::SUSPENDED};
    bool started{false};

    // Snapshot; meaningful only while suspended and started.
    std::vector<Value> stack;
    std::vector<CoroutineFrameSnapshot> frames;
    std::vector<CoroutineHandlerSnapshot> handlers;
    // One defer list per frame in the snapshot, in frame order.
    std::vector<std::vector<Value>> defers;
    std::vector<ObjUpvalue*> openUpvalues; // location offset given by the
                                           // parallel vector below
    std::vector<int> openUpvalueOffsets;
    std::vector<std::uint8_t> resultChecks;
    std::vector<std::uint8_t> resultOverrideSet;
    std::vector<Value> resultOverrides;

    // Set on resume, used by suspend to find this coroutine's slice.
    int activeFrameBase{0};
    Value* activeStackBase{nullptr};
    // The operand-stack top to restore on suspend: the end of the resume
    // native's own callee/args window sit below it. callNative consumes that
    // window after the native returns, so the slice is truncated here, not to
    // activeStackBase (which is the slice's own base).
    Value* activeWindowTop{nullptr};
    // m_reentrantRunDepth at the moment this coroutine was resumed. A yield
    // is legal while the depth is unchanged (the C++ frames below the resume
    // point keep running); it is illegal when a deeper re-entrant run was
    // entered inside the coroutine (a native callback or a defer drain).
    int resumeReentrantDepth{0};

    ObjCoroutine(ObjClass* k, Value fn)
        : Obj(ObjType::COROUTINE), klass(k), callee(fn) {}

#ifdef LOXPP_PROFILE
    // Per-coroutine profiler state. The root coroutine uses Runtime's inline
    // ProfilerData instead; a coroutine allocates its own on first resume.
    std::unique_ptr<ProfilerData> profiler;
    std::vector<std::optional<ProfileFunctionScope>> profilerScopes;
#endif
};

inline bool isObjCoroutine(Obj* o) { return isObjType(o, ObjType::COROUTINE); }
inline ObjCoroutine* asObjCoroutine(Obj* o) {
    return static_cast<ObjCoroutine*>(o);
}
inline ObjCoroutine* asObjCoroutine(const Value& v) {
    return static_cast<ObjCoroutine*>(as<Obj*>(v));
}
inline bool isCoroutine(const Value& v) {
    return isValueOfType<ObjType::COROUTINE>(v);
}
