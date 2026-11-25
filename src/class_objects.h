#pragma once

#include "barrier.h"
#include "exec_objects.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm_allocator.h"

struct ObjClass : public Obj {
    ObjString* const name;

    ObjClass(ObjString* n, VmAllocator<Entry> alloc)
        : Obj(ObjType::CLASS), name(n), m_methods(alloc) {}

    [[nodiscard]] ObjClass* superclass() const { return m_superclass; }
    [[nodiscard]] const Table& methods() const { return m_methods; }

    // The mutators. Each one runs the write barrier (src/barrier.h).
    void setSuperclass(ObjClass* superclass) {
        Collector* c = barrier::collectorOf(m_methods.allocator());
        if (barrier::active(c)) {
            c->barrierField(this, barrier::refValue(m_superclass),
                            barrier::refValue(superclass));
        }
        m_superclass = superclass;
    }
    void setMethod(ObjString* key, const Value& method) {
        barrier::tableSet(barrier::collectorOf(m_methods.allocator()), this,
                          m_methods, key, method, Store::Field);
    }
    // Copies each method of `from` into this class (Op::INHERIT).
    void inheritMethods(const ObjClass& from) {
        from.m_methods.forEach(
            [this](ObjString* key, const Value& v) { setMethod(key, v); });
    }

  private:
    ObjClass* m_superclass{nullptr}; // set by Op::INHERIT
    Table m_methods;
};

inline bool isObjClass(Obj* o) { return isObjType(o, ObjType::CLASS); }
inline ObjClass* asObjClass(Obj* o) { return static_cast<ObjClass*>(o); }
inline bool isClass(const Value& v) {
    return isValueOfType<ObjType::CLASS>(v);
} // NOLINT

struct ObjInstance : public Obj {
    ObjClass* const klass;

    ObjInstance(ObjClass* k, VmAllocator<Entry> alloc)
        : Obj(ObjType::INSTANCE), klass(k), m_fields(alloc) {}

    // ObjString* → Value (arbitrary runtime fields)
    [[nodiscard]] const Table& fields() const { return m_fields; }

    // Returns true when the field is new.
    bool setField(ObjString* key, const Value& value) {
        return barrier::tableSet(barrier::collectorOf(m_fields.allocator()),
                                 this, m_fields, key, value, Store::Field);
    }

  private:
    Table m_fields;
};

inline bool isObjInstance(Obj* o) { return isObjType(o, ObjType::INSTANCE); }
inline ObjInstance* asObjInstance(Obj* o) {
    return static_cast<ObjInstance*>(o);
}
inline bool isInstance(const Value& v) {
    return isValueOfType<ObjType::INSTANCE>(v);
}

struct ObjBoundMethod : public Obj {
    const Value receiver; // the instance this method is bound to
    ObjClosure* const method;

    ObjBoundMethod(Value recv, ObjClosure* m)
        : Obj(ObjType::BOUND_METHOD), receiver(recv), method(m) {}
};

inline bool isObjBoundMethod(Obj* o) {
    return isObjType(o, ObjType::BOUND_METHOD);
}
inline ObjBoundMethod* asObjBoundMethod(Obj* o) {
    return static_cast<ObjBoundMethod*>(o);
}
inline bool isBoundMethod(const Value& v) {
    return isValueOfType<ObjType::BOUND_METHOD>(v);
}
