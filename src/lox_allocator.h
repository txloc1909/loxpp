#pragma once

// lox_allocator.h — STL-compatible allocator adapter over Allocator*.
//
// LoxAllocator<T> satisfies the C++17 minimal allocator concept.  Every
// allocation / deallocation it performs is routed through
// Allocator::reallocate, so m_bytesAllocated is always up-to-date and any
// future GC implementation can track memory uniformly.
//
// Convenience aliases:
//   LoxString   — std::basic_string<char> backed by LoxAllocator<char>
//   LoxVector<T>— std::vector<T>          backed by LoxAllocator<T>
//
// Usage example (inside a future ObjArray):
//   struct ObjArray : public Obj {
//       LoxVector<Value> elements;
//       explicit ObjArray(Allocator* alloc) :
//       elements(LoxAllocator<Value>{alloc}) {}
//   };

#include "allocator.h"

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

template <typename T>
class LoxAllocator {
  public:
    using value_type = T;
    // Propagate allocator on move so containers remain consistent after move.
    using propagate_on_container_move_assignment = std::true_type;
    // Two LoxAllocators are equal iff they share the same Allocator backend.
    using is_always_equal = std::false_type;

    explicit LoxAllocator(Allocator* alloc) noexcept : m_alloc(alloc) {}

    // Rebind copy constructor: LoxAllocator<U> → LoxAllocator<T>.
    template <typename U>
    LoxAllocator(const LoxAllocator<U>&
                     other) noexcept // NOLINT(google-explicit-constructor)
        : m_alloc(other.m_alloc) {}

    T* allocate(std::size_t n) {
        return static_cast<T*>(m_alloc->reallocate(nullptr, 0, n * sizeof(T)));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        m_alloc->reallocate(p, n * sizeof(T), 0);
    }

    // Exposed so the rebind copy constructor can access it across T/U.
    Allocator* m_alloc;
};

template <typename T, typename U>
bool operator==(const LoxAllocator<T>& a, const LoxAllocator<U>& b) noexcept {
    return a.m_alloc == b.m_alloc;
}

template <typename T, typename U>
bool operator!=(const LoxAllocator<T>& a, const LoxAllocator<U>& b) noexcept {
    return !(a == b);
}

// ---------------------------------------------------------------------------
// Convenience type aliases
// ---------------------------------------------------------------------------

using LoxString =
    std::basic_string<char, std::char_traits<char>, LoxAllocator<char>>;

template <typename T>
using LoxVector = std::vector<T, LoxAllocator<T>>;
