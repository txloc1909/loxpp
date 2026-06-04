#pragma once

#include "class_objects.h"
#include "core_hash_map.h"
#include "object.h"
#include "value.h"
#include "vm_allocator.h"

#include <cstdio>

struct ObjList : public Obj {
    VmVector<Value> elements;

    explicit ObjList(VmAllocator<Value> alloc)
        : Obj(ObjType::LIST), elements(alloc) {}
};

inline bool isObjList(Obj* o) { return isObjType(o, ObjType::LIST); }
inline ObjList* asObjList(Obj* o) { return static_cast<ObjList*>(o); }
inline bool isList(const Value& v) { return isValueOfType<ObjType::LIST>(v); }

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
inline bool isFile(const Value& v) { return isValueOfType<ObjType::FILE>(v); }

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
    static uint32_t hashOf(const MapEntry& e); // defined in container_objects.cpp
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
inline bool isMap(const Value& v) { return isValueOfType<ObjType::MAP>(v); }

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
