#pragma once

#include <cstddef>
#include <new>
#include <string>
#include <vector>

// Minimal raw-memory interface. MemoryManager inherits this.
// Lives here (not in memory_manager.h) to break the circular include:
//   vm_allocator.h → (nothing)
//   object.h       → vm_allocator.h
//   memory_manager.h → object.h, table.h → vm_allocator.h
class Collector;

struct VmAllocBase {
    std::size_t bytesAllocated{0};
    // The collector of the heap. A mutator of an object (src/barrier.h) finds
    // it here, through the allocator of the object's own container, so a store
    // needs no extra argument. Null for an allocator that has no collector.
    Collector* barrierCollector{nullptr};
    virtual void* rawAlloc(std::size_t bytes) {
        bytesAllocated += bytes;
        return ::operator new(bytes);
    }
    void rawFree(void* p, std::size_t bytes) noexcept {
        bytesAllocated -= bytes;
        ::operator delete(p);
    }
    virtual ~VmAllocBase() = default;
};

template <typename T>
struct VmAllocator {
    using value_type = T;
    VmAllocBase* mm;

    explicit VmAllocator(VmAllocBase* mm) noexcept : mm(mm) {}

    template <typename U>
    VmAllocator(const VmAllocator<U>& o) noexcept
        : mm(o.mm) {} // NOLINT(google-explicit-constructor)

    T* allocate(std::size_t n) {
        return static_cast<T*>(mm->rawAlloc(n * sizeof(T)));
    }
    void deallocate(T* p, std::size_t n) noexcept {
        mm->rawFree(p, n * sizeof(T));
    }

    bool operator==(const VmAllocator& o) const noexcept { return mm == o.mm; }
    bool operator!=(const VmAllocator& o) const noexcept { return mm != o.mm; }
};

using LoxString =
    std::basic_string<char, std::char_traits<char>, VmAllocator<char>>;

template <typename T>
using VmVector = std::vector<T, VmAllocator<T>>;
