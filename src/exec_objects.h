#pragma once

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
    Value*
        location; // points into stack while open; points to &closed when closed
    Value closed{Nil{}};
    ObjUpvalue* next{nullptr}; // intrusive linked list of open upvalues in VM

    explicit ObjUpvalue(Value* slot) : Obj(ObjType::UPVALUE), location(slot) {}
};

inline bool isObjUpvalue(Obj* o) { return isObjType(o, ObjType::UPVALUE); }
inline ObjUpvalue* asObjUpvalue(Obj* o) { return static_cast<ObjUpvalue*>(o); }

struct ObjClosure : public Obj {
    ObjFunction* function;
    std::vector<ObjUpvalue*> upvalues;
    int upvalueCount;

    explicit ObjClosure(ObjFunction* fn)
        : Obj(ObjType::CLOSURE), function(fn),
          upvalues(fn->upvalueCount, nullptr), upvalueCount(fn->upvalueCount) {}
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
