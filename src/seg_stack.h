#pragma once

// A stack of pointers made of fixed blocks that are linked, not copied. A push
// or a pop touches one block, so no operation costs more than a fixed amount,
// whatever the size of the stack (docs/decisions/garbage-collectors.md, 4.2).
// A std::vector that doubles copies the whole array in one call.
//
// The blocks come from the buffer classes of the heap (SizeClassHeap), so a
// block is a slot of a span: no call of the C allocator, and no accounting
// in bytesAllocated (the collector's own data does not count as program
// memory, and counting it would make an allocation of the collector
// poll the collector again).

#include "size_class_heap.h"

#include <cstddef>
#include <utility>

template <typename T>
class SegStack {
    static constexpr std::size_t kBlockBytes = 2048;
    struct Block {
        Block* prev;
        T items[(kBlockBytes - sizeof(Block*)) / sizeof(T)];
    };
    static constexpr std::size_t kCap =
        (kBlockBytes - sizeof(Block*)) / sizeof(T);
    static_assert(sizeof(Block) <= kBlockBytes);

  public:
    explicit SegStack(SizeClassHeap& store) : m_store(&store) {}
    ~SegStack() { clear(); }
    SegStack(const SegStack&) = delete;
    SegStack& operator=(const SegStack&) = delete;

    [[nodiscard]] bool empty() const { return m_head == nullptr; }
    [[nodiscard]] std::size_t size() const { return m_size; }

    void push(const T& v) {
        if (m_head == nullptr || m_headCount == kCap) {
            Block* b = takeBlock();
            b->prev = m_head;
            m_head = b;
            m_headCount = 0;
        }
        m_head->items[m_headCount++] = v;
        ++m_size;
    }

    // The stack must not be empty.
    T pop() {
        T v = m_head->items[--m_headCount];
        --m_size;
        if (m_headCount == 0) {
            Block* dead = m_head;
            m_head = dead->prev;
            m_headCount = m_head == nullptr ? 0 : kCap;
            giveBlock(dead);
        }
        return v;
    }

    // Frees every block, the spare one too.
    void clear() {
        while (m_head != nullptr) {
            Block* dead = m_head;
            m_head = dead->prev;
            m_store->freeBuffer(dead, sizeof(Block));
        }
        m_headCount = 0;
        m_size = 0;
        if (m_spare != nullptr) {
            m_store->freeBuffer(m_spare, sizeof(Block));
            m_spare = nullptr;
        }
    }

    // Takes the blocks of `other`, which must use the same heap. This stack
    // must be empty.
    void swap(SegStack& other) {
        std::swap(m_head, other.m_head);
        std::swap(m_headCount, other.m_headCount);
        std::swap(m_size, other.m_size);
        std::swap(m_spare, other.m_spare);
    }

  private:
    Block* takeBlock() {
        if (m_spare != nullptr) {
            Block* b = m_spare;
            m_spare = nullptr;
            return b;
        }
        return static_cast<Block*>(m_store->allocBuffer(sizeof(Block)));
    }
    // One spare block stays, so a stack that crosses a block edge back and
    // forth does not allocate and free at each crossing.
    void giveBlock(Block* b) {
        if (m_spare == nullptr) {
            m_spare = b;
        } else {
            m_store->freeBuffer(b, sizeof(Block));
        }
    }

    SizeClassHeap* m_store;
    Block* m_head{nullptr};
    std::size_t m_headCount{0};
    std::size_t m_size{0};
    Block* m_spare{nullptr};
};
