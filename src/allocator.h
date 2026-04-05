#pragma once

#include "object.h"

#include <cstddef>
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
    // Single gateway for all raw heap memory managed by this allocator.
    // - ptr == nullptr, oldSize == 0: fresh allocation of newSize bytes.
    // - newSize == 0: free ptr (oldSize bytes); returns nullptr.
    // - otherwise: resize ptr from oldSize to newSize bytes.
    // Implementations must update their byte-tracking bookkeeping here.
    virtual void* reallocate(void* ptr, size_t oldSize, size_t newSize) = 0;

    virtual ObjHandle makeString(std::string_view chars) = 0;
    // Rvalue overload: takes an already-built std::string, avoiding a redundant
    // copy in the common (non-interned) path.
    // Default implementation falls back to the string_view overload.
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
    // Total bytes currently allocated through this allocator.
    virtual size_t bytesAllocated() const = 0;
    virtual ~Allocator() = default;
};
