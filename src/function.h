#pragma once

#include "chunk.h"
#include "object.h"
#include "value.h"

#include <vector>

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
    return is<Obj*>(v) && as<Obj*>(v)->type == ObjType::CLOSURE;
}
