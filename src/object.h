#pragma once

#include <cstdint>
#include <string>
#include <string_view>

enum class ObjType { STRING };

struct Obj {
    ObjType type;
    bool marked = false;
    virtual ~Obj() = default;
};

struct ObjString : public Obj {
    uint32_t length{0};
    uint32_t hash{0};
    // Null-terminated char buffer allocated and freed via
    // Allocator::reallocate.
    char* chars{nullptr};
};

std::string stringifyObj(Obj* obj);
void printObject(Obj* obj);

inline bool isObjType(Obj* obj, ObjType type) { return obj->type == type; }
inline ObjString* asObjString(Obj* obj) { return static_cast<ObjString*>(obj); }
inline const char* asCString(Obj* obj) {
    return static_cast<ObjString*>(obj)->chars;
}
