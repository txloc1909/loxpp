#pragma once

#include <cstdlib>
#include <limits>
#include <memory>

class AllocatorState {
  public:
    static AllocatorState& getInstance() {
        static AllocatorState instance;
        return instance;
    };

    std::size_t getByteAllocated() const { return m_byteAllocated; }
    void addByteAllocated(std::size_t delta) { m_byteAllocated += delta; }

    void reset() { m_byteAllocated = 0; }

  private:
    AllocatorState() = default;
    ~AllocatorState() = default;
    AllocatorState(const AllocatorState&) = delete;
    AllocatorState& operator=(const AllocatorState&) = delete;

    std::size_t m_byteAllocated = 0;
};

class AllocatorImpl {
  public:
    static constexpr std::size_t growCapacity(std::size_t capacity) {
        return capacity < 8 ? 8 : (capacity * 2);
    };

    template <typename T>
    static T* allocate(int count) {
        return static_cast<T*>(reallocate(NULL, 0, sizeof(T) * count));
    }

    template <typename T>
    static T* growArray(T* pointer, size_t oldCount, size_t newCount) {
        return static_cast<T*>(
            reallocate(pointer, sizeof(T) * oldCount, sizeof(T) * newCount));
    }

    template <typename T>
    static void freeArray(T* pointer, size_t oldCount) {
        reallocate(pointer, sizeof(T) * oldCount, 0);
    }

    static void* reallocate(void* pointer, size_t oldSize, size_t newSize) {
        auto delta = newSize - oldSize;
        if (delta != 0) {
            AllocatorState::getInstance().addByteAllocated(newSize - oldSize);
        }

        if (newSize == 0) {
            std::free(pointer);
            return nullptr;
        }

        auto* result = std::realloc(pointer, newSize);
        if (result == nullptr) {
            throw std::bad_alloc();
        }
        return result;
    }

    static void reset() { AllocatorState::getInstance().reset(); }

  private:
    AllocatorImpl() = delete;
    ~AllocatorImpl() = delete;
};

template <typename T>
class Allocator {
  public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    struct rebind {
        using other = Allocator<U>;
    };

    Allocator() noexcept = default;

    template <typename U>
    Allocator(const Allocator<U>&) noexcept {}

    T* allocate(size_type n) {
        return static_cast<T*>(
            AllocatorImpl::reallocate(nullptr, 0, sizeof(T) * n));
    }

    void deallocate(T* p, size_type n) noexcept {
        AllocatorImpl::reallocate(p, sizeof(T) * n, 0);
    }

    size_type max_size() const noexcept {
        return std::numeric_limits<size_type>::max() / sizeof(T);
    }
};

template <typename T, typename U>
bool operator==(const Allocator<T>&, const Allocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
bool operator!=(const Allocator<T>&, const Allocator<U>&) noexcept {
    return false;
}
