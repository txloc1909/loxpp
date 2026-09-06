#pragma once

#include "object.h"
#include "table.h"
#include "vm_allocator.h"

#include <functional>
#include <string_view>
#include <vector>

#ifdef LOXPP_PROFILE
struct ProfilerData; // defined in profiler.h, included only in
                     // memory_manager.cpp
#endif

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
        // m_stressGC fires a collection on every allocation to surface rooting
        // bugs. It is a runtime flag, not a build flag, so this stays one
        // member read and one `||` on the hot allocation path.
        if (m_stressGC || bytesAllocated > m_nextGC) {
            collectGarbage();
        }
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

    void pushTempRoot(Obj* obj);
    void popTempRoot();

    void setMarkRootsCallback(std::function<void()> cb);
    void setCurrentCompiler(Compiler* c);
#ifdef LOXPP_PROFILE
    void setProfilerData(ProfilerData* data) { m_profilerData = data; }
#endif
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
    std::vector<Obj*> m_tempRoots; // objects transiently protected from GC
    std::function<void()> m_markRoots;
    Compiler* m_currentCompiler{nullptr};
#ifdef LOXPP_PROFILE
    ProfilerData* m_profilerData{nullptr};
#endif
    // Set from the LOXPP_STRESS_GC environment variable, read once in the
    // constructor. When true, every allocation triggers a full collection.
    // When false, m_nextGC growth and the trigger point are exactly as they
    // are with no stress: this flag adds one branch and nothing else.
    bool m_stressGC{false};
    std::size_t m_nextGC{1024 * 1024};
    static constexpr int GC_HEAP_GROW_FACTOR = 2;
};
