#pragma once

#include "object.h"

#include <cstdint>
#include <string>
#include <string_view>

struct ObjHandle {
    uint32_t index;
    ObjType type;
    bool operator==(const ObjHandle& other) const {
        return index == other.index && type == other.type;
    }
    bool operator!=(const ObjHandle& other) const { return !(*this == other); }
};

class Allocator {
  public:
    virtual ObjHandle makeString(std::string_view chars) = 0;
    // Rvalue overload: moves an already-built std::string into the new
    // ObjString, avoiding a second heap allocation in the common (non-interned)
    // case. Default implementation falls back to the string_view overload.
    virtual ObjHandle makeString(std::string&& chars) {
        return makeString(std::string_view{chars});
    }
    // Disambiguate string literals: const char* is an exact match here, so
    // overload resolution never sees the string_view / string&& ambiguity.
    ObjHandle makeString(const char* chars) {
        return makeString(std::string_view{chars});
    }
    // Non-inserting lookup: returns the interned ObjString* if it exists, else
    // nullptr.
    virtual ObjString* findString(std::string_view chars) const = 0;
    virtual Obj* deref(ObjHandle handle) const = 0;
    virtual void collect() = 0;
    virtual ~Allocator() = default;
};
