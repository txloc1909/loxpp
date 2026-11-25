#pragma once

// The helpers that the mutators of the heap object types use to run the write
// barrier (docs/decisions/garbage-collectors.md, section 6). The table of
// the mutators and their barriers is in docs/reference/gc-runtime-contract.md,
// section 7, and tools/check_gc_barrier_table.py checks it.
//
// The rules of a mutator:
//  - It calls the barrier before it overwrites or removes a reference, with
//    the old value and the new value. The barrier reads the old value only
//    while a mark runs (`active`), so the fast path is two loads and a test.
//  - It never calls the barrier inside a growth of a buffer, and it reads
//    the old value after the growth. A growth calls the allocator, and an
//    allocation can collect and start a mark. A store that can grow calls
//    ensureRoom first. Then the barrier and the store make no allocation.
//    An insert has no old reference, so its barrier call passes Nil as the
//    old value.
//  - A removed or overwritten Table key or Map key goes to the barrier as a
//    Value in its own call, before the call for the value. The hooks keep
//    their signature (container, old, new).

#include "collector.h"
#include "table.h"
#include "value.h"

namespace barrier {

template <typename Alloc>
inline Collector* collectorOf(const Alloc& alloc) {
    return alloc.mm->barrierCollector;
}

// True for an object type whose stores have no barrier yet (decision section
// 6, item 3). A collector that needs the barrier traces each object of such a
// type again in its finish pause. A type is listed with a `case` here and has
// a row with the status unprotected in the table of
// docs/reference/gc-runtime-contract.md, section 7. Every type has its
// mutators now, so none is listed.
constexpr bool isUnprotectedType(ObjType type) {
    switch (type) {
    default:
        return false;
    }
}

inline bool active(const Collector* c) { return c != nullptr && c->marking(); }

inline Value nil() { return Value{Nil{}}; }

inline Value refValue(Obj* o) { return o == nullptr ? Value{Nil{}} : Value{o}; }

// A store into a field that holds one object pointer, for a type whose
// object holds no allocator.
template <typename T>
inline void storeRef(Collector& c, Obj* owner, T*& slot, T* value) {
    if (c.marking()) {
        c.barrierField(owner, refValue(slot), refValue(value));
    }
    slot = value;
}

// A store into an entry of a Table that an object owns (the fields of an
// Instance, the methods of a Class). Returns true when the key is new.
inline bool tableSet(Collector* c, Obj* owner, Table& t, ObjString* key,
                     const Value& value, Store kind) {
    // The growth comes first. A collection in it can start a mark, and the
    // old value must be read after that point.
    t.ensureRoom();
    if (active(c)) {
        Value old = nil();
        t.get(key, old);
        if (kind == Store::Field) {
            c->barrierField(owner, old, value);
        } else {
            c->barrierEntry(owner, old, value);
        }
    }
    return t.set(key, value);
}

} // namespace barrier
