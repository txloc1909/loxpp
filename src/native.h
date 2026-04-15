#pragma once

#include "object.h"
#include "value.h"

// Signature for all native (C++) functions callable from Lox.
using NativeFn = Value (*)(int argCount, Value* args);

// Signal a runtime error from inside a native function.
// The VM checks this flag after every native call and surfaces the message.
void nativeRuntimeError(const char* msg);

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
