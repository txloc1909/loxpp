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
    bool marked = false;
    explicit Obj(ObjType t) : type(t) {}
    virtual ~Obj() = default;
};

struct ObjString : public Obj {
    uint32_t hash{0};
    LoxString chars;

    ObjString(std::string_view sv, VmAllocator<char> alloc)
        : Obj(ObjType::STRING), hash(hashString(sv)), chars(sv, alloc) {}
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
