#pragma once

#include "vm_allocator.h"

#include <cstdint>
#include <string>
#include <string_view>

enum class ObjType : uint8_t {
    STRING,
    FUNCTION,
    NATIVE,
    UPVALUE,
    CLOSURE,
    CLASS,
    INSTANCE,
    BOUND_METHOD,
    LIST,
    FILE,
    ITERATOR,
    MAP,
    ENUM_CTOR,
    ENUM,
    BOUND_NATIVE,
    ERROR,
    DEFERRED_CALL,
    SOCKET,
    SERVER,
    PROCESS,
    COROUTINE,
    TASK,
    INT,
};

inline uint32_t hashString(std::string_view s) {
    uint32_t hash = 2166136261U;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 16777619U;
    }
    return hash;
}

struct Obj {
    ObjType type;
    // Written only by the collector of the heap that owns the object. A
    // worker that holds the object of another heap marks it in a side table
    // of its own (MemoryManager::markObject), never here.
    bool marked = false;
    // The id of the heap that created the object (worker.h, heapById). Set
    // once by MemoryManager::create and never changed, so another worker
    // can read it at any time. It sits in the header padding: sizeof(Obj)
    // does not change.
    std::uint16_t heapId = 0;
    explicit Obj(ObjType t) : type(t) {}
    virtual ~Obj() = default;
};

struct ObjString : public Obj {
    uint32_t hash{0};
    LoxString chars;

    ObjString(std::string_view sv, VmAllocator<char> alloc)
        : Obj(ObjType::STRING), hash(hashString(sv)), chars(sv, alloc) {}
};

// An Int that does not fit the native inline range (src/value.h). Immutable:
// no code writes `value` after construction, so the compiler pools one object
// per distinct value and the collector never sees a mutated one.
//
// It lives here, not with the other container objects, because value.h needs
// the type for is<Int>/as<Int> and the container headers include value.h.
struct ObjInt : public Obj {
    std::int64_t value;
    explicit ObjInt(std::int64_t v) : Obj(ObjType::INT), value(v) {}
};

std::string stringifyObj(Obj* obj);

// Maximum recursion depth for stringifying nested structures. Measured crash
// boundaries: list ~1500-2000, map ~1200-1500 (tightest), enum ~1500-2000.
// Set to 200 to maintain 6x safety factor below the tightest boundary (map).
inline constexpr int kMaxStringifyDepth = 200;

inline bool isObjType(Obj* obj, ObjType type) { return obj->type == type; }
inline ObjString* asObjString(Obj* obj) { return static_cast<ObjString*>(obj); }
inline const char* asCString(Obj* obj) {
    return static_cast<ObjString*>(obj)->chars.c_str();
}
