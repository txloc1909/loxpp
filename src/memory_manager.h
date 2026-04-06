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

    // Creates and takes ownership of a new Obj subclass.
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        bytesAllocated += sizeof(T);
        T* p = new T(std::forward<Args>(args)...);
        allObjects.push_back(p);
        return p;
    }

    void release(Obj* obj, std::size_t size);

    // Returns an interned ObjString, creating one if not already present.
    ObjString* makeString(std::string_view sv);
    ObjString* makeString(std::string&& sv);
    ObjString* makeString(const char* chars) {
        return makeString(std::string_view{chars});
    }
    [[nodiscard]] ObjString* findString(std::string_view sv) const;

    void collectAll();

  private:
    // Plain std::vector — using VmAllocator here would cause infinite recursion
    // inside rawAlloc when push_back triggers a reallocation.
    std::vector<Obj*> allObjects;
    Table m_strings;
};
