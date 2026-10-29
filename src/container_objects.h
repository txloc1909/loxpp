#pragma once

#include "class_objects.h"
#include "core_hash_map.h"
#include "object.h"
#include "value.h"
#include "vm_allocator.h"

#include <atomic>
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
// The Socket owns `fd`; close() or the destructor closes it. Do not rely on
// the destructor to close sockets; like ObjFile, GC timing is
// non-deterministic. Call close() explicitly, or use `defer s.close()`.
struct ObjTask;

struct ObjSocket : public Obj {
    ObjClass* klass; // shared Socket class; GC-tracked
    int fd{-1};      // raw descriptor; -1 when closed
    bool writeClosed{false};
    // The tasks parked in write() or writeline() on this socket, in the order
    // their calls began. Only the first one sends, so the bytes of one call
    // are never split by the bytes of another. An entry can be stale (its task
    // no longer waits here); the owner of the queue checks before it uses one.
    // GC-tracked, so a stale entry never dangles. The queue is allocated at
    // the first park: a deque costs about 600 bytes, and most sockets never
    // wait to write. Access it only through the members below.
    std::unique_ptr<std::deque<ObjTask*>> writers;
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

    bool hasWriters() const { return writers && !writers->empty(); }
    bool hasWriterQueue() const { return writers != nullptr; }
    ObjTask* frontWriter() const { return writers->front(); }
    void popWriter() { writers->pop_front(); }
    void pushWriter(ObjTask* task) {
        if (!writers) {
            writers = std::make_unique<std::deque<ObjTask*>>();
        }
        writers->push_back(task);
    }
    void clearWriters() { writers.reset(); }
    std::deque<ObjTask*> takeWriters() {
        std::deque<ObjTask*> out;
        if (writers) {
            out.swap(*writers);
        }
        return out;
    }
    template <typename F>
    void forEachWriter(F&& f) const {
        if (writers) {
            for (ObjTask* t : *writers) {
                f(t);
            }
        }
    }
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
    // Whether this value is a message (spec/03-types.md, Messages): true when
    // every field is a message. The constructor sets it once, after it fills
    // the fields. No construct writes a field after that, so the bit stays
    // valid for the value's whole life and a send reads only this bit.
    bool sendable{true};

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

    // Snapshot; meaningful only while suspended and started. Every vector is
    // on the worker's heap: a large snapshot counts toward the collection
    // threshold, so a suspended coroutine cannot grow the heap unseen.
    VmVector<Value> stack;
    VmVector<CoroutineFrameSnapshot> frames;
    VmVector<CoroutineHandlerSnapshot> handlers;
    // One defer list per frame in the snapshot, in frame order.
    VmVector<VmVector<Value>> defers;
    VmVector<ObjUpvalue*> openUpvalues; // location offset given by the
                                        // parallel vector below
    VmVector<int> openUpvalueOffsets;
    VmVector<std::uint8_t> resultChecks;
    VmVector<std::uint8_t> resultOverrideSet;
    VmVector<Value> resultOverrides;

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

    // A task switch at a receive suspends every coroutine that the task
    // resumed, not only the innermost one (spec/04-semantics.md,
    // task.receive). Each outer coroutine then waits at the call that resumed
    // the next one, and the C++ continuation of that call is gone. These two
    // fields rebuild it: the next resume of this coroutine first resumes
    // waitingInner with the value it was given, then hands the result to the
    // call in progress. With waitingIterator null, that call is a native
    // `resume` and the result replaces its slot; otherwise it is an
    // ITER_HAS_NEXT on that iterator. Null outside a task switch.
    ObjCoroutine* waitingInner{nullptr};
    ObjIterator* waitingIterator{nullptr};

    ObjCoroutine(ObjClass* k, Value fn, VmAllocator<Value> alloc)
        : Obj(ObjType::COROUTINE), klass(k), callee(fn), stack(alloc),
          frames(alloc), handlers(alloc), defers(alloc), openUpvalues(alloc),
          openUpvalueOffsets(alloc), resultChecks(alloc),
          resultOverrideSet(alloc), resultOverrides(alloc) {}

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
    READY,   // in the run queue
    RUNNING, // the scheduler resumed it and it has not suspended yet
    WAITING, // suspended in task.receive() on an empty mailbox
    PARKED,  // suspended in a native call that waits for time or a descriptor
    DEAD,    // its function returned, or the program ended it
};

// What a parked task waits for. The wait is one wait of the worker's Poller.
enum class ParkReason : std::uint8_t {
    NONE,
    FD_READ,
    FD_WRITE,
    TIMER,
};

// A network call that waits for its descriptor and goes on when the descriptor
// is ready. The state of the call lives here and not on a C++ stack, because a
// task that waits has no stack: the task owns the record while it is PARKED.
// The record holds only what the call alone owns or what never changes: the
// operation, the target, the method name, the count, and the bytes that
// read_bytes took out of the socket already. It holds no offset into the
// socket's input buffer, because another task can change that buffer while
// this call waits.
enum class IoOp : std::uint8_t {
    NONE,
    ACCEPT,
    READ,
    READ_BYTES,
    READLINE,
    READLINES,
    WRITE,
    CONNECT,
};

// The state of a connect() between its attempts, defined with the Socket
// natives. No Socket exists until an attempt succeeds, so the state owns the
// descriptor of the attempt in progress and closes it when the record dies.
struct ConnectState;

struct ParkedIo {
    IoOp op{IoOp::NONE};
    // The Socket or Server the call waits on. GC-tracked through the task.
    Obj* target{nullptr};
    // The name of the call in a fault message. A string literal.
    const char* method{""};
    // read_bytes: the count asked for, and the bytes taken so far.
    std::size_t want{0};
    std::string buf;
    // write and writeline: the program's String (GC-tracked through the task,
    // never copied), whether a newline follows it, and how many of its bytes,
    // newline included, the descriptor has taken. Only the socket's writer
    // queue moves the offset, and the String is immutable, so the offset stays
    // valid while the call waits.
    Obj* text{nullptr};
    bool newline{false};
    std::size_t offset{0};
    // connect: the addresses left to try and the attempt in progress. There is
    // no target, because the Socket does not exist yet.
    std::shared_ptr<ConnectState> connect;
};

// The object lives in the heap of its worker. A handle on another worker is
// a pointer to this same object, held as a message: that worker reads only
// `ended` and the heap id, and routes a send through the inbound queue of
// the owner (worker.h). Every other field belongs to the owner's thread.
struct ObjTask : public Obj {
    ObjCoroutine* co;
    // Unbounded by design: a send never waits for room. A bound is a library
    // over the primitive (spec/04-semantics.md, Bounding a Mailbox).
    // On the worker's heap: a push can pass the collection threshold and
    // collect, so the owner roots this task across it (deliverMessage).
    std::deque<Value, VmAllocator<Value>> mailbox;
    TaskState state{TaskState::READY};
    // True while the task waits in task.receive(): its next resume hands it
    // the head of the mailbox as the value of that receive.
    bool receiving{false};
    // Set while the task is PARKED: what it waits for and the Poller token of
    // that wait. A parked task keeps its result and its fault here, not in the
    // mailbox, so task.pending() never sees them.
    ParkReason parkReason{ParkReason::NONE};
    std::uint64_t parkToken{0};
    // The value of the parked call when the task runs again, unless a fault
    // is set. `hasPendingFault` and the strings are plain C++ data.
    Value pendingValue;
    // The network call that the task waits in, while it is PARKED on one.
    ParkedIo io;
    bool hasPendingFault{false};
    std::string faultKind;
    std::string faultMessage;
    // The first resume calls the function with this one argument when
    // `hasStartArg` is set: the message of task.spawn_worker.
    bool hasStartArg{false};
    Value startArg;
    // The scheduler's run of the program this task belongs to. A task of an
    // earlier run (an earlier REPL line) has ended with that run.
    std::uint64_t epoch;
    // The order in which the worker made its tasks, for the deadlock report.
    std::uint64_t serial{0};
    // Set by the owner when the task ends; read by any worker that sends to
    // it (DeadTaskError).
    std::atomic<bool> ended{false};

    ObjTask(ObjCoroutine* c, std::uint64_t e, VmAllocator<Value> alloc)
        : Obj(ObjType::TASK), co(c), mailbox(alloc), epoch(e) {}
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
