#pragma once

#include "object.h"
#include "table.h"
#include "vm_allocator.h"

#include <string_view>
#include <vector>

class MemoryManager : public VmAllocBase {
  public:
    MemoryManager();
    ~MemoryManager();

    // Non-copyable, non-movable (VmAllocator stores 'this' pointer).
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;
    MemoryManager(MemoryManager&&) = delete;
    MemoryManager& operator=(MemoryManager&&) = delete;

    // Creates, tracks, and takes ownership of a new Obj subclass.
    // Header bytes counted here; internal buffers (MyString, VmVector) counted
    // via VmAllocator automatically.
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        bytesAllocated += sizeof(T);
        T* p = new T(std::forward<Args>(args)...);
        allObjects.push_back(p);
        return p;
    }

    // Frees a previously created object (called during GC sweep).
    void release(Obj* obj, std::size_t size);

    // String interning: returns an existing ObjString for the same content, or
    // creates a new one tracked by this manager.
    ObjString* makeString(std::string_view sv);
    ObjString* makeString(std::string&& sv);
    // Disambiguate string literals: const char* is an exact match here, so
    // overload resolution never sees the string_view / string&& ambiguity.
    ObjString* makeString(const char* chars) {
        return makeString(std::string_view{chars});
    }
    [[nodiscard]] ObjString* findString(std::string_view sv) const;

    // Frees every tracked object. Called by destructor; may also be called
    // explicitly for full GC collection.
    void collectAll();

  private:
    // Plain std::vector — tracking this vector itself via VmAllocator would
    // cause rawAlloc → push_back → rawAlloc infinite recursion.
    std::vector<Obj*> allObjects;
    Table m_strings; // interning table; tracked via VmAllocator<Entry>
};
