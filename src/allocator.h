#pragma once

// Forward declarations instead of #include "object.h" to avoid a circular
// dependency (object.h → lox_allocator.h → allocator.h).
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

enum class ObjType : uint8_t;
struct Obj;
struct ObjString;

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
    // Single gateway for all heap memory. nullptr/0/newSize = alloc;
    // ptr/oldSize/0 = free; otherwise resize (memcpy semantics).
    virtual void* reallocate(void* ptr, size_t oldSize, size_t newSize) = 0;

    virtual ObjHandle makeString(std::string_view chars) = 0;
    virtual ObjHandle makeString(std::string&& chars) {
        return makeString(std::string_view{chars});
    }
    ObjHandle makeString(const char* chars) {
        return makeString(std::string_view{chars});
    }
    [[nodiscard]] virtual ObjString*
    findString(std::string_view chars) const = 0;
    [[nodiscard]] virtual Obj* deref(ObjHandle handle) const = 0;

    // GC hooks: VM calls markObject() for each root during mark phase;
    // collect() sweeps unreachable objects.
    virtual void markObject(Obj* obj) = 0;
    virtual void collect() = 0;

    [[nodiscard]] virtual size_t bytesAllocated() const = 0;

    virtual ~Allocator() = default;
};
