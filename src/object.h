#pragma once

#include "lox_allocator.h"

#include <cstdint>
#include <string>
#include <string_view>

enum class ObjType : uint8_t { STRING };

struct Obj {
    ObjType type;
    bool marked = false;
    virtual ~Obj() = default;
};

struct ObjString : public Obj {
    uint32_t hash{0};
    // Null-terminated string whose internal buffer is tracked via LoxAllocator.
    LoxString chars;

    explicit ObjString(Allocator* alloc) : chars(LoxAllocator<char>{alloc}) {}
};

std::string stringifyObj(Obj* obj);
void printObject(Obj* obj);

inline bool isObjType(Obj* obj, ObjType type) { return obj->type == type; }
inline ObjString* asObjString(Obj* obj) { return static_cast<ObjString*>(obj); }
inline const char* asCString(Obj* obj) {
    return static_cast<ObjString*>(obj)->chars.c_str();
}
