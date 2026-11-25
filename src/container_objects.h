#pragma once

#include "barrier.h"
#include "class_objects.h"
#include "core_hash_map.h"
#include "object.h"
#include "value.h"
#include "vm_allocator.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <memory>
#include <optional>
#include <vector>

#ifdef LOXPP_PROFILE
#include "profiler.h"
#endif

struct ObjList : public Obj {
    explicit ObjList(VmAllocator<Value> alloc)
        : Obj(ObjType::LIST), m_elements(alloc) {}

    [[nodiscard]] const VmVector<Value>& elements() const { return m_elements; }

    // The mutators. Each one runs the write barrier (src/barrier.h). A call
    // that grows the buffer can collect, so the caller roots the list and the
    // value first. The barrier of a growing call runs after the growth.
    void push(const Value& v) {
        m_elements.push_back(v);
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierElement(this, barrier::nil(), v);
        }
    }
    // The list must not be empty.
    Value pop() {
        Value last = m_elements.back();
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierElement(this, last, barrier::nil());
        }
        m_elements.pop_back();
        return last;
    }
    void set(std::size_t index, const Value& v) {
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierElement(this, m_elements[index], v);
        }
        m_elements[index] = v;
    }
    // A new slot is Nil. A removed slot goes to the barrier first.
    void resize(std::size_t count) {
        Collector* c = collector();
        if (barrier::active(c)) {
            for (std::size_t i = count; i < m_elements.size(); ++i) {
                c->barrierElement(this, m_elements[i], barrier::nil());
            }
        }
        m_elements.resize(count);
    }
    // Each slot from `index` on gets the value of the next slot, so the
    // barrier sees each of those stores.
    void eraseAt(std::size_t index) {
        Collector* c = collector();
        if (barrier::active(c)) {
            for (std::size_t i = index; i < m_elements.size(); ++i) {
                c->barrierElement(this, m_elements[i],
                                  i + 1 < m_elements.size() ? m_elements[i + 1]
                                                            : barrier::nil());
            }
        }
        m_elements.erase(m_elements.begin() +
                         static_cast<std::ptrdiff_t>(index));
    }

  private:
    [[nodiscard]] Collector* collector() const {
        return barrier::collectorOf(m_elements.get_allocator());
    }
    VmVector<Value> m_elements;
};

inline bool isObjList(Obj* o) { return isObjType(o, ObjType::LIST); }
inline ObjList* asObjList(Obj* o) { return static_cast<ObjList*>(o); }
inline bool isList(const Value& v) { return isValueOfType<ObjType::LIST>(v); }

struct ObjIterator : public Obj {
    // ObjList*, ObjString*, ObjMap*, or ObjCoroutine* being iterated. Set once
    // when the iterator is made.
    const Value collection;
    int index; // current cursor position
    // Map structural version recorded at GET_ITER; -1 unless collection is a
    // Map. Any insert or erase during map iteration is an error, even when a
    // paired erase and insert restore the net size.
    int expectedVersion;

    ObjIterator(Value coll, int idx = 0, int expected = -1)
        : Obj(ObjType::ITERATOR), collection(coll), index(idx),
          expectedVersion(expected), m_current{Nil{}} {}

    // Coroutine mode only, and meaningful while hasCurrent() is true: the
    // value ITER_HAS_NEXT resumed out of the coroutine, cached for ITER_NEXT
    // to push. The resume happens at has-next time because its outcome is what
    // decides whether another element exists at all.
    [[nodiscard]] const Value& current() const { return m_current; }
    [[nodiscard]] bool hasCurrent() const { return m_hasCurrent; }

    // The mutators. Each one runs the write barrier (src/barrier.h).
    void setCurrent(Collector& c, const Value& v) {
        if (c.marking()) {
            c.barrierField(this, m_hasCurrent ? m_current : barrier::nil(), v);
        }
        m_current = v;
        m_hasCurrent = true;
    }
    // Gives the cached value to the caller and clears it.
    Value takeCurrent(Collector& c) {
        Value v = m_current;
        if (c.marking()) {
            c.barrierField(this, m_hasCurrent ? v : barrier::nil(),
                           barrier::nil());
        }
        m_current = Value{Nil{}};
        m_hasCurrent = false;
        return v;
    }

  private:
    Value m_current;
    bool m_hasCurrent{false};
};

inline bool isObjIterator(Obj* o) { return isObjType(o, ObjType::ITERATOR); }
inline ObjIterator* asObjIterator(Obj* o) {
    return static_cast<ObjIterator*>(o);
}
inline bool isIterator(const Value& v) {
    return isValueOfType<ObjType::ITERATOR>(v);
}

struct ObjFile : public Obj {
    ObjClass* const klass; // shared s_fileClass; GC-tracked
    FILE* handle{nullptr}; // null when closed
    bool readable{false};
    bool writable{false};
    // The bytes read from the stream and not yet returned to the program. A
    // block read takes more than the one line `readline` returns, and a
    // stream that cannot seek cannot give those bytes back: on a FIFO, a
    // pipe, or /dev/stdin the seek fails, so they must be kept here.
    std::string inbuf;

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
// The Socket owns `fd`; close() or the destructor closes it. Do not rely on
// the destructor to close sockets; like ObjFile, GC timing is
// non-deterministic. Call close() explicitly, or use `defer s.close()`.
struct ObjTask;

struct ObjSocket : public Obj {
    ObjClass* const klass; // shared Socket class; GC-tracked
    int fd{-1};            // raw descriptor; -1 when closed
    bool writeClosed{false};
    // The task whose call holds each side (spec/04-semantics.md, The Task
    // Machine, Sides), or null. The pointer only finds the task: the task's
    // waiting call (ObjTask::io) says whether it still holds the side, so a
    // pointer that outlives the hold is never read as a hold. GC-tracked.
    [[nodiscard]] ObjTask* readHolder() const { return m_readHolder; }
    [[nodiscard]] ObjTask* writeHolder() const { return m_writeHolder; }
    // The mutators. Each one runs the write barrier (src/barrier.h).
    void setReadHolder(Collector& c, ObjTask* task) {
        barrier::storeRef(c, this, m_readHolder, task);
    }
    void setWriteHolder(Collector& c, ObjTask* task) {
        barrier::storeRef(c, this, m_writeHolder, task);
    }

  private:
    ObjTask* m_readHolder{nullptr};
    ObjTask* m_writeHolder{nullptr};

  public:
    // Bytes read from the descriptor and not yet returned. A read may take
    // more than the line it returns; the rest stays here for the next read.
    std::string inbuf;
    // The first `noNewlineLen` bytes of `inbuf` hold no newline, so a
    // readline that resumes scans from here. Every removal from the front of
    // `inbuf` must lower it by the bytes removed (not below zero), and an
    // insertion at the front must set it to zero.
    size_t noNewlineLen = 0;
    // The bytes that readline has searched for a newline, counted so a test
    // can see the cost of a line that arrives in pieces.
    size_t scannedBytes = 0;

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
    ObjClass* const klass; // shared Server class; GC-tracked
    int fd{-1};            // listening descriptor; -1 when closed
    int boundPort{0};
    // The task whose call holds the accept side, as ObjSocket::readHolder.
    [[nodiscard]] ObjTask* acceptHolder() const { return m_acceptHolder; }
    // The mutator. It runs the write barrier (src/barrier.h).
    void setAcceptHolder(Collector& c, ObjTask* task) {
        barrier::storeRef(c, this, m_acceptHolder, task);
    }

  private:
    ObjTask* m_acceptHolder{nullptr};

  public:
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
    ObjClass* const klass; // shared Process class; GC-tracked
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
    ObjClass* const klass;    // shared s_errorClass; GC-tracked
    ObjString* const message; // human-readable error message
    ObjString* const kind;    // error category string

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
    using Entries = CoreHashMap<MapEntry, MapPolicy, VmAllocator<MapEntry>>;

    ObjClass* const klass; // shared s_mapClass, for method dispatch

  private:
    // Declared here so that the layout (and the byte count of the heap) is
    // the one the trace tools expect.
    Entries m_map;

  public:
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
        : Obj(ObjType::MAP), klass(k), m_map(alloc) {}

    [[nodiscard]] const Entries& entries() const { return m_map; }

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

  private:
    [[nodiscard]] Collector* collector() const {
        return barrier::collectorOf(m_map.allocator());
    }
    // A removed or overwritten entry. The key and the value go to the barrier
    // in two calls, because a hook reports one old value and one new value.
    void barrierRemoved(Collector* c, const MapEntry& old);
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
    ObjString* const ctorName;
    ObjString* const enumName;

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
    ObjEnumCtor* const ctor;
    // Whether this value is a message (spec/03-types.md, Messages): true when
    // every field is a message. The constructor sets it once, after it fills
    // the fields. No construct writes a field after that, so the bit stays
    // valid for the value's whole life and a send reads only this bit.
    bool sendable{true};

    ObjEnum(ObjEnumCtor* c, VmAllocator<Value> alloc)
        : Obj(ObjType::ENUM), ctor(c), m_fields(alloc) {}

    [[nodiscard]] const VmVector<Value>& fields() const { return m_fields; }

    // The mutators. Each one runs the write barrier (src/barrier.h). The
    // constructor of the value calls them while it fills the fields, and
    // none after that. A new slot is Nil, so the barrier sees no old
    // reference.
    void resize(std::size_t count) {
        Collector* c = barrier::collectorOf(m_fields.get_allocator());
        if (barrier::active(c)) {
            for (std::size_t i = count; i < m_fields.size(); ++i) {
                c->barrierElement(this, m_fields[i], barrier::nil());
            }
        }
        m_fields.resize(count);
    }
    void set(std::size_t index, const Value& v) {
        Collector* c = barrier::collectorOf(m_fields.get_allocator());
        if (barrier::active(c)) {
            c->barrierElement(this, m_fields[index], v);
        }
        m_fields[index] = v;
    }

  private:
    VmVector<Value> m_fields;
};

inline bool isEnumValue(const Value& v) {
    return isValueOfType<ObjType::ENUM>(v);
}
inline ObjEnum* asObjEnum(Obj* o) { return static_cast<ObjEnum*>(o); }

// ---------------------------------------------------------------------------
// ObjDeferredCall — captured callable and its arguments, pending invocation
// ---------------------------------------------------------------------------
struct ObjDeferredCall : public Obj {
    // The function to call. Set once when the object is made.
    const Value callable;

    ObjDeferredCall(Value fn, VmAllocator<Value> alloc)
        : Obj(ObjType::DEFERRED_CALL), callable(fn), m_args(alloc) {}

    // The arguments to pass.
    [[nodiscard]] const VmVector<Value>& args() const { return m_args; }

    // The mutator. It runs the write barrier (src/barrier.h) after the
    // growth of the buffer.
    void addArg(const Value& v) {
        m_args.push_back(v);
        Collector* c = barrier::collectorOf(m_args.get_allocator());
        if (barrier::active(c)) {
            c->barrierElement(this, barrier::nil(), v);
        }
    }

  private:
    VmVector<Value> m_args;
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
    ObjClass* const klass; // shared Coroutine class; GC-tracked
    const Value callee;    // Function or BoundMethod to run; set once
    CoroutineState state{CoroutineState::SUSPENDED};
    bool started{false};

  private:
    // The data members keep the order of the layout that the byte counts of
    // the trace tools were measured with.
    VmVector<Value> m_stack;
    VmVector<CoroutineFrameSnapshot> m_frames;
    VmVector<CoroutineHandlerSnapshot> m_handlers;
    VmVector<VmVector<Value>> m_defers;
    VmVector<ObjUpvalue*> m_openUpvalues;
    VmVector<int> m_openUpvalueOffsets;
    VmVector<std::uint8_t> m_resultChecks;
    VmVector<std::uint8_t> m_resultOverrideSet;
    VmVector<Value> m_resultOverrides;

  public:
    // Set on resume, used by suspend to find this coroutine's slice.
    int activeFrameBase{0};
    Value* activeStackBase{nullptr};
    // The operand-stack top to restore on suspend: the end of the resume
    // native's own callee/args window sit below it. callNative consumes that
    // window after the native returns, so the slice is truncated here, not to
    // activeStackBase (which is the slice's own base).
    Value* activeWindowTop{nullptr};
    // TaskOwned::reentrantRunDepth at the moment this coroutine was resumed. A
    // yield is legal while the depth is unchanged (the C++ frames below the
    // resume point keep running); it is illegal when a deeper re-entrant run
    // was entered inside the coroutine (a native callback or a defer drain).
    int resumeReentrantDepth{0};

  private:
    ObjCoroutine* m_waitingInner{nullptr};
    ObjIterator* m_waitingIterator{nullptr};

  public:
    ObjCoroutine(ObjClass* k, Value fn, VmAllocator<Value> alloc)
        : Obj(ObjType::COROUTINE), klass(k), callee(fn), m_stack(alloc),
          m_frames(alloc), m_handlers(alloc), m_defers(alloc),
          m_openUpvalues(alloc), m_openUpvalueOffsets(alloc),
          m_resultChecks(alloc), m_resultOverrideSet(alloc),
          m_resultOverrides(alloc) {}

    // The snapshot; meaningful only while suspended and started. Every vector
    // is on the worker's heap: a large snapshot counts toward the collection
    // threshold, so a suspended coroutine cannot grow the heap unseen.
    [[nodiscard]] const VmVector<Value>& stack() const { return m_stack; }
    [[nodiscard]] const VmVector<CoroutineFrameSnapshot>& frames() const {
        return m_frames;
    }
    [[nodiscard]] const VmVector<CoroutineHandlerSnapshot>& handlers() const {
        return m_handlers;
    }
    // One defer list per frame in the snapshot, in frame order.
    [[nodiscard]] const VmVector<VmVector<Value>>& defers() const {
        return m_defers;
    }
    // The location offset of each is in the parallel vector below.
    [[nodiscard]] const VmVector<ObjUpvalue*>& openUpvalues() const {
        return m_openUpvalues;
    }
    [[nodiscard]] const VmVector<int>& openUpvalueOffsets() const {
        return m_openUpvalueOffsets;
    }
    [[nodiscard]] const VmVector<std::uint8_t>& resultChecks() const {
        return m_resultChecks;
    }
    [[nodiscard]] const VmVector<std::uint8_t>& resultOverrideSet() const {
        return m_resultOverrideSet;
    }
    [[nodiscard]] const VmVector<Value>& resultOverrides() const {
        return m_resultOverrides;
    }

    // A task switch at a receive suspends every coroutine that the task
    // resumed, not only the innermost one (spec/04-semantics.md,
    // task.receive). Each outer coroutine then waits at the call that resumed
    // the next one, and the C++ continuation of that call is gone. These two
    // fields rebuild it: the next resume of this coroutine first resumes
    // waitingInner with the value it was given, then hands the result to the
    // call in progress. With waitingIterator null, that call is a native
    // `resume` and the result replaces its slot; otherwise it is an
    // ITER_HAS_NEXT on that iterator. Null outside a task switch.
    [[nodiscard]] ObjCoroutine* waitingInner() const { return m_waitingInner; }
    [[nodiscard]] ObjIterator* waitingIterator() const {
        return m_waitingIterator;
    }

    // The mutators. Each one runs the write barrier (src/barrier.h), kind
    // Snapshot. A mutator that grows a buffer calls the barrier after the
    // growth.
    //
    // The snapshot goes back to the shared stack (a resume): every reference
    // in it is dropped, so each goes to the barrier first.
    void clearSnapshot() {
        Collector* c = collector();
        if (barrier::active(c)) {
            for (const Value& v : m_stack) {
                c->barrierSnapshot(this, v, barrier::nil());
            }
            for (const auto& f : m_frames) {
                c->barrierSnapshot(this, barrier::refValue(f.closure),
                                   barrier::nil());
            }
            for (const auto& list : m_defers) {
                for (const Value& v : list) {
                    c->barrierSnapshot(this, v, barrier::nil());
                }
            }
            for (ObjUpvalue* uv : m_openUpvalues) {
                c->barrierSnapshot(this, barrier::refValue(uv), barrier::nil());
            }
            for (const Value& v : m_resultOverrides) {
                c->barrierSnapshot(this, v, barrier::nil());
            }
        }
        m_stack.clear();
        m_frames.clear();
        m_handlers.clear();
        m_defers.clear();
        m_openUpvalues.clear();
        m_openUpvalueOffsets.clear();
        m_resultChecks.clear();
        m_resultOverrideSet.clear();
        m_resultOverrides.clear();
    }
    // The snapshot is empty (clearSnapshot ran before).
    void captureStack(const Value* first, const Value* last) {
        m_stack.assign(first, last);
        Collector* c = collector();
        if (barrier::active(c)) {
            for (const Value& v : m_stack) {
                c->barrierSnapshot(this, barrier::nil(), v);
            }
        }
    }
    void reserveFrames(std::size_t count) { m_frames.reserve(count); }
    void pushFrame(const CoroutineFrameSnapshot& frame) {
        m_frames.push_back(frame);
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierSnapshot(this, barrier::nil(),
                               barrier::refValue(frame.closure));
        }
    }
    void pushHandler(const CoroutineHandlerSnapshot& handler) {
        m_handlers.push_back(handler);
    }
    // Copies one defer list.
    template <typename It>
    void pushDefers(It first, It last) {
        m_defers.push_back(
            VmVector<Value>(first, last, m_defers.get_allocator()));
        Collector* c = collector();
        if (barrier::active(c)) {
            for (const Value& v : m_defers.back()) {
                c->barrierSnapshot(this, barrier::nil(), v);
            }
        }
    }
    void pushResult(std::uint8_t check, bool overrideSet,
                    const Value& overrideValue) {
        m_resultChecks.push_back(check);
        m_resultOverrideSet.push_back(overrideSet ? 1 : 0);
        m_resultOverrides.push_back(overrideValue);
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierSnapshot(this, barrier::nil(), overrideValue);
        }
    }
    void reserveOpenUpvalues(std::size_t count) {
        m_openUpvalueOffsets.reserve(count);
        m_openUpvalues.reserve(count);
    }
    void pushOpenUpvalue(ObjUpvalue* uv, int offset) {
        m_openUpvalueOffsets.push_back(offset);
        m_openUpvalues.push_back(uv);
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierSnapshot(this, barrier::nil(), barrier::refValue(uv));
        }
    }
    // Points each open upvalue at its cell in this snapshot. A cell is a root
    // before and a slot of this object after, and the value does not change,
    // so no barrier runs.
    void bindOpenUpvalues() {
        for (std::size_t i = 0; i < m_openUpvalues.size(); i++) {
            m_openUpvalues[i]->setLocation(m_stack.data() +
                                           m_openUpvalueOffsets[i]);
        }
    }
    // The task switch of a receive (see waitingInner).
    void setWaiting(ObjCoroutine* inner, ObjIterator* iter) {
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierSnapshot(this, barrier::refValue(m_waitingInner),
                               barrier::refValue(inner));
            c->barrierSnapshot(this, barrier::refValue(m_waitingIterator),
                               barrier::refValue(iter));
        }
        m_waitingInner = inner;
        m_waitingIterator = iter;
    }

  private:
    [[nodiscard]] Collector* collector() const {
        return barrier::collectorOf(m_stack.get_allocator());
    }

  public:
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

// ---------------------------------------------------------------------------
// ObjTask — a task on this worker (spec/03-types.md, §Task;
// spec/04-semantics.md, Workers and Tasks).
// ---------------------------------------------------------------------------
// A task runs its function in its own coroutine. The worker's scheduler
// (Runtime::runTasks) resumes that coroutine; a suspend point of the task is a
// suspension of it. A Task value is the handle: identity equality, always a
// message.
enum class TaskState : std::uint8_t {
    READY,     // in the run queue
    RUNNING,   // the scheduler resumed it and it has not suspended yet
    RECEIVING, // suspended in task.receive() on an empty mailbox
    WAITING,   // suspended in a native call that waits for time or a
               // descriptor, or in a receive with a limit (the spec calls that
               // task `receiving`: see ObjTask::inStateReceiving)
    ENDED,     // its function returned, or the program ended it
};

// What a WAITING task waits for. The wait is one wait of the worker's Poller.
enum class WaitEvent : std::uint8_t {
    NONE,
    FD_READ,
    FD_WRITE,
    TIMER,
};

// A network call that waits for its descriptor and goes on when the descriptor
// is ready. The state of the call lives here and not on a C++ stack, because a
// task that waits has no stack: the task owns the record from the first wait
// of the call until the task runs again. The record holds what the call alone
// owns or what never changes: the operation, the target, the method name, the
// count, and the bytes that read_bytes took out of the socket already. The
// state of the socket's input buffer, which outlives the call, stays in the
// socket.
enum class IoOp : std::uint8_t {
    NONE,
    ACCEPT,
    READ,
    READ_BYTES,
    READ_SOME,
    READLINE,
    READLINES,
    WRITE,
    CONNECT,
};

// The state of a connect() between its attempts, defined with the Socket
// natives. No Socket exists until an attempt succeeds, so the state owns the
// descriptor of the attempt in progress and closes it when the record dies.
struct ConnectState;

struct WaitingCall {
    IoOp op{IoOp::NONE};
    // The Socket or Server the call waits on. GC-tracked through the task.
    Obj* target{nullptr};
    // The name of the call in a fault message. A string literal.
    const char* method{""};
    // read_bytes and read_some: the count asked for, and the bytes taken so
    // far.
    std::size_t want{0};
    std::string buf;
    // write and writeline: the program's String (GC-tracked through the task,
    // never copied), whether a newline follows it, and how many of its bytes,
    // newline included, the descriptor has taken. Only this call moves the
    // offset, and the String is immutable, so the offset stays valid while the
    // call waits.
    Obj* text{nullptr};
    bool newline{false};
    std::size_t offset{0};
    // connect: the addresses left to try and the attempt in progress. There is
    // no target, because the Socket does not exist yet.
    std::shared_ptr<ConnectState> connect;
    // connect with a limit: the time at which the call fails with a
    // NetworkError that carries `timeoutMessage`.
    bool hasDeadline{false};
    std::chrono::steady_clock::time_point deadline{};
    std::string timeoutMessage;
};

// The state that belongs to one task and that no other task may see
// (spec/04-semantics.md, The Task Machine). The worker keeps one record for
// the code that runs outside every task, and `Runtime::m_owned` points at the
// record of the task that runs now.
struct TaskOwned {
    // True for the duration of one StackOverflowError's own handleThrow()
    // call (set and cleared by two plain assignments, at both overflow
    // guards). Draining a discarded frame's defer runs arbitrary Lox++ code
    // (runPendingDefers() -> a nested run()), and that code can itself
    // recurse deep enough to reach either overflow guard again, because the
    // reserve lets the unwind of a StackOverflowError run deferred calls at
    // all. Without this flag, each such nested hit re-enters handleThrow()'s
    // own unwind through genuine C++ recursion (call() ->
    // raiseThrowableError() -> handleThrow() -> runPendingDefers() -> a
    // nested run() -> call() -> ...), one level per remaining frame, and
    // crashes the process with a real native stack overflow instead of a
    // Lox++-level fault. Both overflow guards skip the catchable path while
    // this flag is set, and fall to their own hard ceiling (FRAMES_MAX or
    // STACK_MAX plus the reserve): the second overflow stays fatal, it is
    // never handed to the same handler a second time.
    bool unwindingStackOverflow{false};
    // Greater than 0 while a `want` function of a receive_where call runs. A
    // receive in it would remove a message from the mailbox that the call
    // is testing.
    int selectDepth{0};
    // Number of natives running now (callNative), innermost included.
    int nativeDepth{0};
    // Number of C++ re-entrant runs on the native stack now: every
    // runReentrantFrame() and runPushedFrameToCompletion() invocation, and
    // every SynchronousRun. A Lox frame inside one of these cannot be
    // captured by a coroutine snapshot: its C++ continuation would be lost.
    // Op::YIELD raises YieldAcrossNativeError while this is above the depth
    // at which the coroutine was resumed.
    int reentrantRunDepth{0};
    // nativeDepth at which the frames of the current coroutine run, while the
    // chain from the running task's base to that coroutine is clean: each
    // link is a `resume` native that its parent's loop called (one native
    // deeper) or an ITER_HAS_NEXT (no native deeper), with no re-entrant run
    // (reentrantRunDepth) in between. -1 when the chain is not clean
    // or no task runs. resumeCoroutine() sets it for its own run and restores
    // it.
    int cleanNativeDepth{-1};
    // reentrantRunDepth when the scheduler resumed the task.
    int cleanReentrantDepth{0};
};

// The object lives in the heap of its worker. A handle on another worker is
// a pointer to this same object, held as a message: that worker reads only
// `ended` and the heap id, and routes a send through the inbound queue of
// the owner (worker.h). Every other field belongs to the owner's thread.
struct ObjTask : public Obj {
    TaskState state{TaskState::READY};
    TaskOwned owned;
    // True while the task waits in task.receive(): its next resume hands it
    // the head of the mailbox as the value of that receive.
    bool receiving{false};
    // Set while the task is WAITING: what it waits for and the Poller token of
    // that wait. A WAITING task keeps its result and its fault here, not in the
    // mailbox, so task.pending() never sees them.
    WaitEvent waitEvent{WaitEvent::NONE};
    std::uint64_t waitToken{0};
    // The Poller token of the limit of a connect call that waits for a
    // descriptor, or 0. The limit is a second wait of the same task.
    std::uint64_t waitTimerToken{0};
    // The network call that the task waits in. It is set at the first wait
    // of the call and kept until the task runs again, so it also names the
    // call while the task is READY after its wait ended: such a call still
    // holds its side (spec/04-semantics.md, The Task Machine, Sides, rule 1).
  private:
    WaitingCall m_io;

  public:
    // The state of a receive_within, receive_where, or receive_where_within
    // call (spec/04-semantics.md, task.receive_within). A task that waits in
    // one has no result to hand over when it runs again: the runtime runs the
    // call again, in the task's own frames, and `pendingRetry` says so. The
    // call keeps how many messages it has tested, so a message is tested once.
    struct SelectiveReceive {
        bool pendingRetry{false};
        bool hasWant{false};
        std::size_t scanned{0};
        bool limited{false};
        std::chrono::steady_clock::time_point deadline{};
        // Set when the wait of the limit was reported. The call still tests
        // the messages that arrived, and raises TimeoutError only if none
        // matches.
        bool timedOut{false};
    };
    SelectiveReceive sel;
    bool hasPendingFault{false};
    std::string faultKind;
    std::string faultMessage;
    // The first resume calls the function with this one argument when
    // `hasStartArg` is set: the message of task.spawn_worker.
    bool hasStartArg{false};
    // The scheduler's run of the program this task belongs to. A task of an
    // earlier run (an earlier REPL line) has ended with that run.
    std::uint64_t epoch;
    // The order in which the worker made its tasks, for the deadlock report.
    std::uint64_t serial{0};
    // Set by the owner when the task ends; read by any worker that sends to
    // it (DeadTaskError).
    std::atomic<bool> ended{false};

    // True when the task is `receiving` in the Task Machine of the spec
    // (spec/04-semantics.md, The Task Machine). A receive with a limit waits
    // on the timer of its limit, so its task is WAITING here, and `receiving`
    // in the spec.
    [[nodiscard]] bool inStateReceiving() const {
        return state == TaskState::RECEIVING ||
               (state == TaskState::WAITING && sel.pendingRetry);
    }

    ObjTask(ObjCoroutine* c, std::uint64_t e, VmAllocator<Value> alloc)
        : Obj(ObjType::TASK), epoch(e), m_co(c), m_mailbox(alloc) {}

    [[nodiscard]] ObjCoroutine* co() const { return m_co; }
    // The network call that the task waits in (see the comment above).
    [[nodiscard]] const WaitingCall& io() const { return m_io; }
    // The record, for the step functions of a call that waits many times.
    // They write the progress of the call (the bytes read, the offset, the
    // state of a connect). They never write `target` or `text`: those two
    // are set on a record that no task owns yet, and a task gets them only
    // through setWait.
    WaitingCall& ioProgress() { return m_io; }
    // The task starts to wait in `call`, or stops (an empty record). Each
    // one runs the write barrier (src/barrier.h) for the objects that the
    // old record names.
    void setWait(WaitingCall&& call) {
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierField(this, barrier::refValue(m_io.target),
                            barrier::refValue(call.target));
            c->barrierField(this, barrier::refValue(m_io.text),
                            barrier::refValue(call.text));
        }
        m_io = std::move(call);
    }
    void clearWait() { setWait(WaitingCall{}); }
    // Unbounded by design: a send never waits for room. A bound is a library
    // over the primitive (spec/04-semantics.md, Bounding a Mailbox).
    // On the worker's heap: a push can pass the collection threshold and
    // collect, so the owner roots this task across it (deliverMessage).
    [[nodiscard]] const std::deque<Value, VmAllocator<Value>>& mailbox() const {
        return m_mailbox;
    }
    // The value of the waiting call when the task runs again, unless a fault
    // is set. `hasPendingFault` and the strings are plain C++ data.
    [[nodiscard]] const Value& pendingValue() const { return m_pendingValue; }
    [[nodiscard]] const Value& startArg() const { return m_startArg; }
    // The test function of a receive_where call (see SelectiveReceive).
    [[nodiscard]] const Value& selectWant() const { return m_selectWant; }

    // The mutators. Each one runs the write barrier (src/barrier.h).
    void setCo(ObjCoroutine* co) {
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierField(this, barrier::refValue(m_co),
                            barrier::refValue(co));
        }
        m_co = co;
    }
    // A push can grow the buffer and collect, so the barrier runs after it.
    void pushMessage(const Value& v) {
        m_mailbox.push_back(v);
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierElement(this, barrier::nil(), v);
        }
    }
    // The mailbox must not be empty.
    Value popMessage() {
        Value v = m_mailbox.front();
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierElement(this, v, barrier::nil());
        }
        m_mailbox.pop_front();
        return v;
    }
    // Each message after `index` moves one place to the front, so the
    // barrier sees each store.
    void eraseMessage(std::size_t index) {
        Collector* c = collector();
        if (barrier::active(c)) {
            for (std::size_t i = index; i < m_mailbox.size(); ++i) {
                c->barrierElement(this, m_mailbox[i],
                                  i + 1 < m_mailbox.size() ? m_mailbox[i + 1]
                                                           : barrier::nil());
            }
        }
        m_mailbox.erase(m_mailbox.begin() + static_cast<std::ptrdiff_t>(index));
    }
    void clearMailbox() {
        Collector* c = collector();
        if (barrier::active(c)) {
            for (const Value& v : m_mailbox) {
                c->barrierElement(this, v, barrier::nil());
            }
        }
        m_mailbox.clear();
    }
    void setPendingValue(const Value& v) { storeValue(m_pendingValue, v); }
    void setStartArg(const Value& v) { storeValue(m_startArg, v); }
    void setSelectWant(const Value& v) { storeValue(m_selectWant, v); }
    // The receive_where state goes back to its start, and drops its test
    // function.
    void resetSelect() {
        sel = SelectiveReceive{};
        setSelectWant(Value{});
    }

  private:
    [[nodiscard]] Collector* collector() const {
        return barrier::collectorOf(m_mailbox.get_allocator());
    }
    void storeValue(Value& slot, const Value& v) {
        Collector* c = collector();
        if (barrier::active(c)) {
            c->barrierField(this, slot, v);
        }
        slot = v;
    }

    ObjCoroutine* m_co;
    std::deque<Value, VmAllocator<Value>> m_mailbox;
    Value m_pendingValue;
    Value m_startArg;
    Value m_selectWant;
};

inline bool isObjTask(Obj* o) { return isObjType(o, ObjType::TASK); }
inline ObjTask* asObjTask(Obj* o) { return static_cast<ObjTask*>(o); }
inline bool isTask(const Value& v) { return isValueOfType<ObjType::TASK>(v); }

// The message rule (spec/03-types.md, Messages). Nil, Boolean, Number,
// String, and Task are always messages; an Enum value carries its bit; every
// other kind is never one. Constant time: an Enum's bit was fixed when it was
// constructed.
inline bool isMessage(const Value& v) {
    // An Int is always a message, inline or heap-held. It is checked before
    // isObj() because a heap-held one is an object, and no other object kind
    // is always a message.
    if (is<Int>(v)) {
        return true;
    }
    if (!isObj(v)) {
        return true;
    }
    if (isString(v) || isTask(v)) {
        return true;
    }
    if (isEnumValue(v)) {
        return asObjEnum(as<Obj*>(v))->sendable;
    }
    return false;
}
