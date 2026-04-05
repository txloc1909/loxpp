#pragma once

// allocator.h uses only forward declarations of Obj/ObjString/ObjType so that
// lox_allocator.h can include us without cycling back through object.h.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Forward declarations — full definitions live in object.h.
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
    // Single gateway for all raw heap memory managed by this allocator.
    // - ptr == nullptr, oldSize == 0: fresh allocation of newSize bytes.
    // - newSize == 0: free ptr (oldSize bytes); returns nullptr.
    // - otherwise: resize ptr from oldSize to newSize (memcpy semantics).
    // Implementations must update byte-tracking bookkeeping here.
    virtual void* reallocate(void* ptr, size_t oldSize, size_t newSize) = 0;

    virtual ObjHandle makeString(std::string_view chars) = 0;
    // Rvalue overload: avoids redundant copy when the caller already has a
    // std::string. Default falls back to the string_view overload.
    virtual ObjHandle makeString(std::string&& chars) {
        return makeString(std::string_view{chars});
    }
    // Disambiguate string literals so overload resolution is unambiguous.
    ObjHandle makeString(const char* chars) {
        return makeString(std::string_view{chars});
    }
    // Non-inserting lookup: returns the interned ObjString* or nullptr.
    [[nodiscard]] virtual ObjString*
    findString(std::string_view chars) const = 0;
    [[nodiscard]] virtual Obj* deref(ObjHandle handle) const = 0;

    // GC interface ---------------------------------------------------------
    // Mark a single reachable object (called by the VM during the mark phase).
    // Implementations set obj->marked and schedule sub-object traversal.
    virtual void markObject(Obj* obj) = 0;
    // Sweep: free all unmarked objects, reset marks on live ones.
    virtual void collect() = 0;

    // Total bytes currently managed by this allocator.
    [[nodiscard]] virtual size_t bytesAllocated() const = 0;

    virtual ~Allocator() = default;
};
