#pragma once

#include <cstdio>

#include "chunk.h"
#include "object.h"
#include "table.h"
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

struct ObjClass : public Obj {
    ObjString* name;
    Table methods; // ObjString* → ObjClosure* (populated in future chapters)

    ObjClass(ObjString* n, VmAllocator<Entry> alloc)
        : Obj(ObjType::CLASS), name(n), methods(alloc) {}
};

inline bool isObjClass(Obj* o) { return isObjType(o, ObjType::CLASS); }
inline ObjClass* asObjClass(Obj* o) { return static_cast<ObjClass*>(o); }
inline bool isClass(const Value& v) {
    return is<Obj*>(v) && as<Obj*>(v)->type == ObjType::CLASS;
}

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
    return is<Obj*>(v) && as<Obj*>(v)->type == ObjType::INSTANCE;
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
    return is<Obj*>(v) && as<Obj*>(v)->type == ObjType::BOUND_METHOD;
}

struct ObjList : public Obj {
    VmVector<Value> elements;

    explicit ObjList(VmAllocator<Value> alloc)
        : Obj(ObjType::LIST), elements(alloc) {}
};

inline bool isObjList(Obj* o) { return isObjType(o, ObjType::LIST); }
inline ObjList* asObjList(Obj* o) { return static_cast<ObjList*>(o); }
inline bool isList(const Value& v) {
    return is<Obj*>(v) && as<Obj*>(v)->type == ObjType::LIST;
}

struct ObjFile : public Obj {
    ObjClass* klass;       // shared s_fileClass; GC-tracked
    FILE* handle{nullptr}; // null when closed
    bool readable{false};
    bool writable{false};

    explicit ObjFile(ObjClass* k) : Obj(ObjType::FILE), klass(k) {}
    ~ObjFile() override {
        if (handle) {
            std::fclose(handle);
            handle = nullptr;
        }
    }
};

inline bool isObjFile(Obj* o) { return isObjType(o, ObjType::FILE); }
inline ObjFile* asObjFile(Obj* o) { return static_cast<ObjFile*>(o); }
inline bool isFile(const Value& v) {
    return is<Obj*>(v) && as<Obj*>(v)->type == ObjType::FILE;
}
