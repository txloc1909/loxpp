#pragma once

#include "chunk.h"
#include "object.h"
#include "value.h"

struct ObjFunction : public Obj {
    int arity{0};
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
