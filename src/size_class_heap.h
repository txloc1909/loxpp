#pragma once

// The memory of one heap (MemoryManager): size classes, mark bitmaps, and an
// allocator that owns its free lists
// (docs/decisions/garbage-collectors.md, phase 8).
//
// Objects and buffers live in spans of 64 KiB. A span holds slots of one size
// class, and the span of a pointer is `ptr & ~(kSpanSize - 1)`. A block larger
// than the largest class has its own allocation from the C allocator, in a
// size class of the form m * 2^k.
//
// What this layout guarantees:
//   - A small allocation or free of the heap's own memory (an object slot or
//     a VmAllocator buffer of at most kMaxSmall bytes) never calls the C
//     allocator or the kernel.
//   - Every call that the heap itself makes to the C allocator or the kernel
//     is timed and bounded: a new chunk (operator new, one call of a fixed
//     size), the map of a large block (mmap), and the release of a large
//     block (munmap, in slices of kReleaseSlice at most, one slice for each
//     step; see releaseStep). The timed calls write the `stall` records.
//   - A span, once taken from the C allocator, stays until the heap ends. An
//     empty span goes to a pool that every size class takes from.
//   - No operation walks or copies an array whose size follows the heap size.
//     A free list is intrusive, the lists of spans are linked through the span
//     headers, and the bitmaps have a fixed size for each span.
//   - Objects never move.
// What it does not cover: the std:: containers that an object or the
// collector owns (a closure's upvalue vector, a function's Chunk, a String
// member of a container object, the gray stack, the tables of the heap).
// They call the C allocator directly and nothing times them. The contract
// document, section 1, lists them.

#include "object.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>

class SizeClassHeap {
  public:
    static constexpr std::size_t kSpanSize = std::size_t{1} << 16;
    static constexpr std::size_t kChunkSpans = 16;
    // The largest block that a span serves. A larger request is a large block.
    static constexpr std::size_t kMaxSmall = 2048;
    static constexpr std::size_t kMaxObject = kMaxSmall;
    // The most that one call gives back to the kernel.
    static constexpr std::size_t kReleaseSlice = std::size_t{1} << 20;
    // The most that one call of releaseBytes gives back.
    static constexpr std::size_t kReleaseBudget = 64 * kReleaseSlice;
    static constexpr std::size_t kSlotAlign = 16;
    // The default limit of a stall record, in nanoseconds.
    static constexpr std::uint64_t kDefaultStallLimitNs = 100'000;

    SizeClassHeap();
    // Returns every chunk and every cached large block to the C allocator.
    // Every block that a caller still holds is gone with it.
    ~SizeClassHeap();
    SizeClassHeap(const SizeClassHeap&) = delete;
    SizeClassHeap& operator=(const SizeClassHeap&) = delete;

    // --- Objects ---------------------------------------------------------

    // The memory for an object of `size` bytes, at most kMaxObject. The slot
    // is not an object of the heap until commitObject: a collection that runs
    // while the constructor runs does not see it.
    [[nodiscard]] void* allocObject(std::size_t size);
    // The constructor of the object at `p` has finished. The sweep sees it
    // from now on, unmarked.
    void commitObject(void* p);
    // Gives the slot of an object back. The caller has run the destructor, or
    // the constructor has failed (then the slot is not committed).
    void freeObject(void* p, bool committed = true);
    // The number of committed objects.
    [[nodiscard]] std::size_t objectCount() const { return m_objects; }

    // --- The mark bit ----------------------------------------------------
    // `obj` must be an object of this heap.

    [[nodiscard]] bool isMarked(const Obj* obj) const;
    // Sets the mark; true when it was clear.
    bool tryMark(const Obj* obj);

    // --- The sweep, one span at a time ----------------------------------

    struct Span;
    // The object spans, in a list that only grows. nullptr ends it.
    [[nodiscard]] Span* firstObjectSpan() const { return m_objectSpans; }
    [[nodiscard]] static Span* nextObjectSpan(const Span* s);
    // Frees each committed, unmarked object of `s`: onDead(obj) runs first,
    // then the destructor, then the slot goes to its free list. Clears the
    // marks of `s`. Returns the number of objects that live on. A destructor
    // may free a buffer, never an object.
    // Stamps the span as swept in the current sweep (beginSweep).
    template <typename OnDead>
    std::size_t sweepSpan(Span* s, OnDead&& onDead);
    // A sweep that goes through the spans in more than one step starts here.
    // Every span is unswept until sweepSpan visits it, and a span that the heap
    // takes after this call counts as swept, because the sweep does not need
    // to visit it. The sweep is by epoch, not by a flag in each span, so the
    // start costs one increment, whatever the number of spans.
    void beginSweep() { ++m_sweepEpoch; }
    [[nodiscard]] bool spanSwept(const Span* s) const;
    // True when `obj` is in a span that the current sweep has not visited.
    [[nodiscard]] bool isUnswept(const Obj* obj) const;
    // Clears every mark bit. A collection that stops before its sweep uses it.
    void clearAllMarks();
    // Calls fn(obj, marked) for each committed object, in no set order.
    template <typename Fn>
    void forEachObject(Fn&& fn);
    // Destroys every committed object.
    void destroyAllObjects();

    // --- Buffers (VmAllocator memory) ------------------------------------

    [[nodiscard]] void* allocBuffer(std::size_t bytes);
    void freeBuffer(void* p, std::size_t bytes) noexcept;

    // --- Observation -----------------------------------------------------

    // The bytes that the heap holds from the C allocator: the chunks, the
    // large blocks in use and the cached large blocks.
    [[nodiscard]] std::size_t footprintBytes() const {
        return m_chunkBytes + m_largeLiveBytes + m_largeCachedBytes +
               m_largePendingBytes;
    }
    [[nodiscard]] std::size_t chunkCount() const { return m_chunkCount; }
    // The number of calls into the C allocator and the kernel that the heap
    // has made (and timed).
    [[nodiscard]] std::size_t osCalls() const { return m_osCalls; }
    // The largest number of bytes that one call has given back to the kernel.
    // It is at most kReleaseSlice, except for the memory that the heap gives
    // back when it ends.
    [[nodiscard]] std::size_t largestReleaseBytes() const {
        return m_largestRelease;
    }
    // The bytes of large blocks that wait to go back to the kernel.
    [[nodiscard]] std::size_t pendingReleaseBytes() const {
        return m_largePendingBytes;
    }
    // Gives back at most kReleaseSlice bytes of a freed large block, or moves
    // one cached block that is over the cache limit to the blocks that wait.
    // The allocation paths and the sweep call it, so a large release is
    // spread over many steps.
    // Returns the bytes that it gave back.
    // With evictCache, a cached block also goes to the queue when the queue is
    // empty, over the cache limit or not: the caller maps new memory, and the
    // cached blocks of other classes are the least useful memory.
    std::size_t releaseStep(bool evictCache = false);
    // Gives back `bytes` from the queue, in calls of at most kReleaseSlice, or
    // all of it when the queue holds less. One call gives back at most
    // kReleaseBudget bytes; the rest stays queued. The heap calls it before it
    // maps new memory, so the queue falls at least as fast as the mappings
    // grow.
    void releaseBytes(std::size_t bytes);
    // The `stall` records go to `trace`, one for each timed call that takes
    // longer than the limit. nullptr turns them off, and then a timed call
    // reads no clock.
    void setStallTrace(std::FILE* trace, std::uint64_t limitNs);

  private:
    struct ClassInfo {
        std::uint32_t size;
        std::uint32_t slots;
        std::uint32_t slotsOffset;
        std::uint32_t bitmapWords; // 0 for a buffer class
        std::uint32_t reciprocal;
    };
    struct SizeClass {
        Span* partial{
            nullptr}; // spans with a free slot, in a doubly linked list
    };
    enum class Kind : std::uint8_t { Pooled, Object, Buffer };

    static constexpr std::size_t kClasses = 24;
    static constexpr std::size_t kLargeClasses = 4 * 48;

    static std::size_t smallClass(std::size_t bytes);
    static std::size_t largeClass(std::size_t bytes, std::size_t& rounded);

    Span* takeSpan(Kind kind, std::size_t cls);
    void* allocSlow(Kind kind, std::size_t cls);
    void* popSlot(Span* s);
    void linkPartial(Span* s);
    void unlinkPartial(Span* s);
    // Gives an empty span back to the pool, unless it is the only span with a
    // free slot in its class.
    void releaseIfEmpty(Span* s);
    void freeSlot(Span* s, void* p);
    Span* carveSpan();
    void* osNew(std::size_t bytes);
    void osDelete(void* p) noexcept;
    void* osMap(std::size_t bytes);
    void osUnmap(void* p, std::size_t bytes) noexcept;
    // A freed large block, over the cache limit: it waits for releaseStep.
    void queueRelease(void* p, std::size_t bytes) noexcept;
    void* allocLarge(std::size_t bytes);
    void freeLarge(void* p, std::size_t bytes) noexcept;
    void noteStall(const char* kind, std::uint64_t t0, std::uint64_t t1);
    static std::uint64_t nowNs();

    SizeClass m_objClasses[kClasses];
    SizeClass m_bufClasses[kClasses];
    Span* m_objectSpans{nullptr};
    Span* m_pool{nullptr};
    // The chunks, linked through their first bytes.
    void* m_chunks{nullptr};
    char* m_nextSpan{nullptr};
    char* m_chunkEnd{nullptr};
    void* m_largeFree[kLargeClasses]{};
    std::size_t m_largeLiveBytes{0};
    std::size_t m_largeCachedBytes{0};
    std::size_t m_largePendingBytes{0};
    std::size_t m_largestRelease{0};
    struct ReleaseNode;
    ReleaseNode* m_pending{nullptr};
    std::size_t m_chunkBytes{0};
    std::size_t m_chunkCount{0};
    std::size_t m_objects{0};
    std::uint32_t m_sweepEpoch{0};
    std::size_t m_osCalls{0};
    std::FILE* m_stallTrace{nullptr};
    std::uint64_t m_stallLimitNs{kDefaultStallLimitNs};
};

// The header of a span. It sits at the start of the span, so the span of a
// pointer is found with a mask. The bitmaps of an object span follow it.
struct SizeClassHeap::Span {
    Span* partialNext;
    Span* partialPrev;
    Span* objectNext; // the list of object spans; never unlinked
    void* freeHead;   // intrusive: the first word of a free slot
    std::uint32_t reciprocal;
    // The sweep epoch of the last sweep that visited the span, or of the
    // epoch in which the heap took the span (SizeClassHeap::beginSweep).
    std::uint32_t sweepEpoch;
    std::uint16_t bump;
    std::uint16_t used; // slots handed out, committed or not
    std::uint16_t slotSize;
    std::uint16_t slots;
    std::uint16_t slotsOffset;
    std::uint16_t cls;
    std::uint16_t bitmapWords;
    std::uint8_t kind;
    bool inPartial;
    bool listed; // in the list of object spans
    static constexpr std::size_t kHeaderBytes = 64;

    std::uint64_t* marks() {
        return reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(this) +
                                                kHeaderBytes);
    }
    std::uint64_t* allocs() { return marks() + bitmapWords; }
    char* slot(std::uint32_t i) {
        return reinterpret_cast<char*>(this) + slotsOffset +
               static_cast<std::size_t>(i) * slotSize;
    }
    // The index of the slot that holds `p`. The division is a multiply: with
    // an offset below 2^16 and a size at most 2^11 the product is exact.
    std::uint32_t indexOf(const void* p) const {
        std::uint32_t off = static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(p) -
            reinterpret_cast<std::uintptr_t>(this) - slotsOffset);
        return static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(off) * reciprocal) >> 32);
    }
};
static_assert(sizeof(SizeClassHeap::Span) <= SizeClassHeap::Span::kHeaderBytes);

inline SizeClassHeap::Span* SizeClassHeap::nextObjectSpan(const Span* s) {
    return s->objectNext;
}

namespace size_class_heap_detail {
inline SizeClassHeap::Span* spanOf(const void* p) {
    return reinterpret_cast<SizeClassHeap::Span*>(
        reinterpret_cast<std::uintptr_t>(p) & ~(SizeClassHeap::kSpanSize - 1));
}
} // namespace size_class_heap_detail

inline bool SizeClassHeap::spanSwept(const Span* s) const {
    return s->sweepEpoch == m_sweepEpoch;
}

inline bool SizeClassHeap::isUnswept(const Obj* obj) const {
    return size_class_heap_detail::spanOf(obj)->sweepEpoch != m_sweepEpoch;
}

inline bool SizeClassHeap::isMarked(const Obj* obj) const {
    Span* s = size_class_heap_detail::spanOf(obj);
    std::uint32_t i = s->indexOf(obj);
    return (s->marks()[i >> 6] >> (i & 63)) & 1;
}

inline bool SizeClassHeap::tryMark(const Obj* obj) {
    Span* s = size_class_heap_detail::spanOf(obj);
    std::uint32_t i = s->indexOf(obj);
    std::uint64_t& word = s->marks()[i >> 6];
    std::uint64_t bit = std::uint64_t{1} << (i & 63);
    if (word & bit) {
        return false;
    }
    word |= bit;
    return true;
}

template <typename OnDead>
std::size_t SizeClassHeap::sweepSpan(Span* s, OnDead&& onDead) {
    if (s->kind != static_cast<std::uint8_t>(Kind::Object)) {
        return 0;
    }
    std::uint64_t* marks = s->marks();
    std::uint64_t* allocs = s->allocs();
    std::size_t live = 0;
    for (std::uint32_t w = 0; w < s->bitmapWords; ++w) {
        std::uint64_t dead = allocs[w] & ~marks[w];
        live += static_cast<std::size_t>(
            __builtin_popcountll(allocs[w] & marks[w]));
        while (dead != 0) {
            std::uint32_t i =
                w * 64 + static_cast<std::uint32_t>(__builtin_ctzll(dead));
            dead &= dead - 1;
            Obj* obj = reinterpret_cast<Obj*>(s->slot(i));
            onDead(obj);
            obj->~Obj();
            allocs[w] &= ~(std::uint64_t{1} << (i & 63));
            --m_objects;
            freeSlot(s, obj);
        }
        marks[w] = 0;
    }
    s->sweepEpoch = m_sweepEpoch;
    releaseIfEmpty(s);
    return live;
}

template <typename Fn>
void SizeClassHeap::forEachObject(Fn&& fn) {
    for (Span* s = m_objectSpans; s != nullptr; s = s->objectNext) {
        if (s->kind != static_cast<std::uint8_t>(Kind::Object)) {
            continue;
        }
        for (std::uint32_t w = 0; w < s->bitmapWords; ++w) {
            std::uint64_t bits = s->allocs()[w];
            while (bits != 0) {
                std::uint32_t i =
                    w * 64 + static_cast<std::uint32_t>(__builtin_ctzll(bits));
                bits &= bits - 1;
                bool marked = (s->marks()[w] >> (i & 63)) & 1;
                fn(reinterpret_cast<Obj*>(s->slot(i)), marked);
            }
        }
    }
}
