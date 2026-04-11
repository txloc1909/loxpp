#pragma once

#include "object.h"
#include "table.h"
#include "vm_allocator.h"

#include <functional>
#include <string_view>
#include <vector>

class Compiler;

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
        if (bytesAllocated > m_nextGC)
            collectGarbage();
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

    void* rawAlloc(std::size_t bytes) override;

    void setMarkRootsCallback(std::function<void()> cb);
    void setCurrentCompiler(Compiler* c);
    void markObject(Obj* obj);
    void markValue(const Value& v);
    void collectGarbage();

  private:
    void traceReferences();
    void traceObject(Obj* obj);
    void removeWhiteStrings();
    void sweep();

    // Plain std::vector — using VmAllocator here would cause infinite recursion
    // inside rawAlloc when push_back triggers a reallocation.
    std::vector<Obj*> allObjects;
    Table m_strings;

    std::vector<Obj*> m_grayStack;
    std::function<void()> m_markRoots;
    Compiler* m_currentCompiler{nullptr};
#ifdef LOXPP_STRESS_GC
    std::size_t m_nextGC{0};
#else
    std::size_t m_nextGC{1024 * 1024};
#endif
    static constexpr int GC_HEAP_GROW_FACTOR = 2;
};
