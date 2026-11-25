#pragma once

#include "barrier.h"
#include "chunk.h"
#include "object.h"
#include "value.h"
#include "vm_allocator.h"

#include <vector>

// Signature for all native (C++) functions callable from Lox.
using NativeFn = Value (*)(int argCount, Value* args);

// Signal a runtime error from inside a native function.
// The VM checks this flag after every native call and surfaces the message.
void nativeRuntimeError(const char* msg);

struct ObjFunction : public Obj {
    int arity{0};
    int upvalueCount{0};
    ObjString* name{nullptr}; // null for top-level script
    Chunk chunk;

    // The QBE backend's compiled entry point for this function, attached by
    // rt_attach_code (backend/rt_capi.h) after startup's embed-and-recompile
    // rebuild verifies this ObjFunction still matches what the code was
    // generated against (id, arity, chunk-bytes hash). Null for every
    // function no --target qbe program has attached code to, which today is
    // every function: the interpreter and the JVM backend never set
    // it and never read it.
    void* code{nullptr};

    // The position of the top-level `fun` declaration that made this
    // function, among the top-level `fun` declarations of its source, in
    // source order; -1 for every other function. Two compiles of one source
    // give the same ordinals, so a new worker finds "the function of the
    // same declaration" by it (task.spawn_worker), also when two
    // declarations share a name. `sourceId` names that source
    // (Runtime::m_sources).
    int topLevelOrdinal{-1};
    int sourceId{0};

    ObjFunction() : Obj(ObjType::FUNCTION) {}
};

inline bool isObjFunction(Obj* obj) {
    return isObjType(obj, ObjType::FUNCTION);
}
inline ObjFunction* asObjFunction(Obj* obj) {
    return static_cast<ObjFunction*>(obj);
}
inline ObjFunction* asObjFunction(const Value& v) {
    return static_cast<ObjFunction*>(as<Obj*>(v));
}

struct ObjUpvalue : public Obj {
    // The intrusive list of the open upvalues of the VM. It is not a
    // reference that the collector traces: the open upvalues are roots.
    ObjUpvalue* next{nullptr};

    explicit ObjUpvalue(Value* slot)
        : Obj(ObjType::UPVALUE), m_location(slot) {}

    // Points into the stack while open, at the cell in the snapshot of a
    // suspended coroutine while owned, and at the closed value once closed.
    [[nodiscard]] const Value* location() const { return m_location; }
    [[nodiscard]] const Value& closed() const { return m_closed; }
    // While this upvalue's cell lives inside a suspended coroutine's stack
    // snapshot, the coroutine, so marking the upvalue (through an escaping
    // closure) also keeps the snapshot alive. Null otherwise.
    [[nodiscard]] Obj* owner() const { return m_owner; }
    [[nodiscard]] const Value& get() const { return *m_location; }

    // The mutators. Each one runs the write barrier (src/barrier.h).
    //
    // A store through the upvalue. The cell is a root (the value stack), the
    // closed value, or a cell of the snapshot of the owner.
    void set(Collector& c, const Value& v) {
        if (c.marking()) {
            if (m_location == &m_closed) {
                c.barrierUpvalue(this, m_closed, v);
            } else if (m_owner != nullptr) {
                c.barrierSnapshot(m_owner, *m_location, v);
            }
        }
        *m_location = v;
    }
    // The cell of the stack that the upvalue points at goes out of scope: the
    // value moves into the upvalue. No reference is overwritten, except the
    // owner.
    void close(Collector& c) {
        m_closed = *m_location;
        m_location = &m_closed;
        setOwner(c, nullptr);
    }
    void setOwner(Collector& c, Obj* owner) {
        if (c.marking()) {
            c.barrierUpvalue(this, barrier::refValue(m_owner),
                             barrier::refValue(owner));
        }
        m_owner = owner;
    }
    // Moves the cell. The value does not change, and the old and the new cell
    // are both roots or both kept alive by the owner, so no barrier runs.
    void setLocation(Value* slot) { m_location = slot; }

  private:
    Value* m_location;
    Value m_closed{Nil{}};
    Obj* m_owner{nullptr};
};

inline bool isObjUpvalue(Obj* o) { return isObjType(o, ObjType::UPVALUE); }
inline ObjUpvalue* asObjUpvalue(Obj* o) { return static_cast<ObjUpvalue*>(o); }

struct ObjClosure : public Obj {
    ObjFunction* const function;
    int upvalueCount;

    explicit ObjClosure(ObjFunction* fn)
        : Obj(ObjType::CLOSURE), function(fn), upvalueCount(fn->upvalueCount),
          m_upvalues(fn->upvalueCount, nullptr) {}

    [[nodiscard]] const std::vector<ObjUpvalue*>& upvalues() const {
        return m_upvalues;
    }

    // The mutator. It runs the write barrier (src/barrier.h).
    void setUpvalue(Collector& c, std::size_t index, ObjUpvalue* uv) {
        if (c.marking()) {
            c.barrierElement(this, barrier::refValue(m_upvalues[index]),
                             barrier::refValue(uv));
        }
        m_upvalues[index] = uv;
    }

  private:
    std::vector<ObjUpvalue*> m_upvalues;
};

inline bool isObjClosure(Obj* o) { return isObjType(o, ObjType::CLOSURE); }
inline ObjClosure* asObjClosure(Obj* o) { return static_cast<ObjClosure*>(o); }
inline ObjClosure* asObjClosure(const Value& v) {
    return static_cast<ObjClosure*>(as<Obj*>(v));
}
inline bool isClosure(const Value& v) {
    return isValueOfType<ObjType::CLOSURE>(v);
}

struct ObjNative : public Obj {
    NativeFn function;
    int arity; // expected argument count; -1 = variadic (any count accepted)
    ObjNative(NativeFn fn, int arity)
        : Obj(ObjType::NATIVE), function(fn), arity(arity) {}
};

inline bool isObjNative(Obj* obj) { return isObjType(obj, ObjType::NATIVE); }
inline ObjNative* asObjNative(Obj* obj) { return static_cast<ObjNative*>(obj); }
inline ObjNative* asObjNative(const Value& v) {
    return static_cast<ObjNative*>(as<Obj*>(v));
}

struct ObjBoundNative : public Obj {
    const Value receiver;
    ObjNative* const native;
    ObjBoundNative(Value recv, ObjNative* n)
        : Obj(ObjType::BOUND_NATIVE), receiver(recv), native(n) {}
};

inline bool isObjBoundNative(Obj* obj) {
    return isObjType(obj, ObjType::BOUND_NATIVE);
}
inline ObjBoundNative* asObjBoundNative(Obj* obj) {
    return static_cast<ObjBoundNative*>(obj);
}
inline ObjBoundNative* asObjBoundNative(const Value& v) {
    return static_cast<ObjBoundNative*>(as<Obj*>(v));
}
