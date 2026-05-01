#pragma once

#include <cstdio>

#include "chunk.h"
#include "core_hash_map.h"
#include "object.h"
#include "table.h"
#include "value.h"

#include <vector>

// Generic type-checking template for Value types
template <ObjType T>
inline bool isValueOfType(const Value& v) {
    return is<Obj*>(v) && as<Obj*>(v)->type == T;
}

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

struct ObjList : public Obj {
    VmVector<Value> elements;

    explicit ObjList(VmAllocator<Value> alloc)
        : Obj(ObjType::LIST), elements(alloc) {}
};

inline bool isObjList(Obj* o) { return isObjType(o, ObjType::LIST); }
inline ObjList* asObjList(Obj* o) { return static_cast<ObjList*>(o); }
inline bool isList(const Value& v) {
    return isValueOfType<ObjType::LIST>(v);
}

struct ObjIterator : public Obj {
    Value collection; // ObjList*, ObjString*, or ObjMap* being iterated
    int index;        // current cursor position

    ObjIterator(Value coll, int idx = 0)
        : Obj(ObjType::ITERATOR), collection(coll), index(idx) {}
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
    // TODO(loctx): Do NOT rely on this destructor to close file handles.
    // GC timing is non-deterministic; an open file may not be collected until
    // program exit (or never, if the heap is not exhausted). Always call
    // f.close() explicitly.
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
    return isValueOfType<ObjType::FILE>(v);
}

// ---------------------------------------------------------------------------
// ObjMap — open-addressing hash map with Value keys and values.
// Keys must be Bool, Number, Nil, or String (interned).
// ---------------------------------------------------------------------------

enum class MapSlot : uint8_t { EMPTY, OCCUPIED, TOMBSTONE };

struct MapEntry {
    Value key{Nil{}};
    Value value{Nil{}};
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
    static uint32_t hashOf(const MapEntry& e); // defined in object.cpp
    static bool keyMatch(const MapEntry& slot, const MapEntry& needle) {
        return slot.key == needle.key;
    }
};

struct ObjMap : public Obj {
    ObjClass* klass; // shared s_mapClass, for method dispatch
    CoreHashMap<MapEntry, MapPolicy, VmAllocator<MapEntry>> map;

    ObjMap(ObjClass* k, VmAllocator<MapEntry> alloc)
        : Obj(ObjType::MAP), klass(k), map(alloc) {}

    // Returns true if key was newly inserted (false = update).
    bool mapSet(const Value& key, const Value& value);
    bool mapGet(const Value& key, Value& out) const;
    // Returns true if key was found and removed.
    bool mapDel(const Value& key);
};

inline bool isObjMap(Obj* o) { return isObjType(o, ObjType::MAP); }
inline ObjMap* asObjMap(Obj* o) { return static_cast<ObjMap*>(o); }
inline bool isMap(const Value& v) {
    return isValueOfType<ObjType::MAP>(v);
}

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

    ObjEnum(ObjEnumCtor* c, VmAllocator<Value> alloc)
        : Obj(ObjType::ENUM), ctor(c), fields(alloc) {}
};

inline bool isEnumValue(const Value& v) {
    return isValueOfType<ObjType::ENUM>(v);
}
inline ObjEnum* asObjEnum(Obj* o) { return static_cast<ObjEnum*>(o); }
