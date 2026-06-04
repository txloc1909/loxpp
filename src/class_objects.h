#pragma once

#include "exec_objects.h"
#include "object.h"
#include "table.h"
#include "value.h"
#include "vm_allocator.h"

struct ObjClass : public Obj {
    ObjString* name;
    ObjClass* superclass{nullptr}; // set by Op::INHERIT
    Table methods;

    ObjClass(ObjString* n, VmAllocator<Entry> alloc)
        : Obj(ObjType::CLASS), name(n), methods(alloc) {}
};

inline bool isObjClass(Obj* o) { return isObjType(o, ObjType::CLASS); }
inline ObjClass* asObjClass(Obj* o) { return static_cast<ObjClass*>(o); }
inline bool isClass(const Value& v) {
    return isValueOfType<ObjType::CLASS>(v);
} // NOLINT

struct ObjInstance : public Obj {
    ObjClass* klass;
    Table fields; // ObjString* → Value (arbitrary runtime fields)

    ObjInstance(ObjClass* k, VmAllocator<Entry> alloc)
        : Obj(ObjType::INSTANCE), klass(k), fields(alloc) {}
};

inline bool isObjInstance(Obj* o) { return isObjType(o, ObjType::INSTANCE); }
inline ObjInstance* asObjInstance(Obj* o) {
    return static_cast<ObjInstance*>(o);
}
inline bool isInstance(const Value& v) {
    return isValueOfType<ObjType::INSTANCE>(v);
}

struct ObjBoundMethod : public Obj {
    Value receiver; // the instance this method is bound to
    ObjClosure* method;

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
