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
struct VmAllocBase {
    std::size_t bytesAllocated{0};
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
