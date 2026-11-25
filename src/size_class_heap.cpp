#include "size_class_heap.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

constexpr std::size_t kClassSizes[24] = {
    16,  32,  48,  64,  80,  96,  112, 128,  160,  192,  224,  256,
    320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048};

constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kSpanBytes = SizeClassHeap::kSpanSize;

// Cached large blocks stay below the larger of the large bytes in use and
// this floor. A block over the limit goes back to the C allocator at once.
constexpr std::size_t kLargeCacheFloor = std::size_t{4} << 20;

struct ClassTable {
    std::uint8_t byGranule[SizeClassHeap::kMaxSmall / 16 + 1];
    std::uint32_t slots[2][24];
    std::uint32_t slotsOffset[2][24];
    std::uint32_t words[2][24];
    std::uint32_t reciprocal[24];

    ClassTable() {
        std::size_t c = 0;
        for (std::size_t g = 0; g <= SizeClassHeap::kMaxSmall / 16; ++g) {
            while (kClassSizes[c] < g * 16) {
                ++c;
            }
            byGranule[g] = static_cast<std::uint8_t>(c);
        }
        for (std::size_t k = 0; k < 24; ++k) {
            std::size_t size = kClassSizes[k];
            reciprocal[k] =
                static_cast<std::uint32_t>((std::uint64_t{1} << 32) / size) + 1;
            for (int buffer = 0; buffer < 2; ++buffer) {
                std::size_t n = (kSpanBytes - kHeaderBytes) / size;
                std::size_t w = 0;
                std::size_t off = kHeaderBytes;
                for (;;) {
                    w = buffer ? 0 : (n + 63) / 64;
                    off = (kHeaderBytes + 2 * w * 8 + 15) & ~std::size_t{15};
                    if (off + n * size <= kSpanBytes) {
                        break;
                    }
                    --n;
                }
                slots[buffer][k] = static_cast<std::uint32_t>(n);
                slotsOffset[buffer][k] = static_cast<std::uint32_t>(off);
                words[buffer][k] = static_cast<std::uint32_t>(w);
            }
        }
    }
};

const ClassTable& table() {
    static const ClassTable t;
    return t;
}

} // namespace

using Span = SizeClassHeap::Span;

std::uint64_t SizeClassHeap::nowNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

SizeClassHeap::SizeClassHeap() { (void)table(); }

SizeClassHeap::~SizeClassHeap() {
    m_stallTrace = nullptr;
    for (void*& head : m_largeFree) {
        while (head != nullptr) {
            void* next = *static_cast<void**>(head);
            osDelete(head);
            head = next;
        }
    }
    while (m_chunks != nullptr) {
        void* next = *static_cast<void**>(m_chunks);
        osDelete(m_chunks);
        m_chunks = next;
    }
}

void SizeClassHeap::setStallTrace(std::FILE* trace, std::uint64_t limitNs) {
    m_stallTrace = trace;
    m_stallLimitNs = limitNs;
}

void SizeClassHeap::noteStall(const char* kind, std::uint64_t t0,
                              std::uint64_t t1) {
    if (t1 - t0 < m_stallLimitNs) {
        return;
    }
    // Flushed per line, like the `gc` records, so a process that stops
    // without stdio cleanup still leaves every stall so far.
    std::fprintf(m_stallTrace, "stall kind=%s t0=%llu t1=%llu\n", kind,
                 static_cast<unsigned long long>(t0),
                 static_cast<unsigned long long>(t1));
    std::fflush(m_stallTrace);
}

void* SizeClassHeap::osNew(std::size_t bytes) {
    ++m_osCalls;
    if (m_stallTrace == nullptr) {
        return ::operator new(bytes);
    }
    std::uint64_t t0 = nowNs();
    void* p = ::operator new(bytes);
    noteStall("new", t0, nowNs());
    return p;
}

void SizeClassHeap::osDelete(void* p) noexcept {
    ++m_osCalls;
    if (m_stallTrace == nullptr) {
        ::operator delete(p);
        return;
    }
    std::uint64_t t0 = nowNs();
    ::operator delete(p);
    noteStall("free", t0, nowNs());
}

std::size_t SizeClassHeap::smallClass(std::size_t bytes) {
    return table().byGranule[(bytes + 15) >> 4];
}

std::size_t SizeClassHeap::largeClass(std::size_t bytes, std::size_t& rounded) {
    std::size_t b = bytes - 1;
    int hb = 63 - __builtin_clzll(b);
    std::size_t m = b >> (hb - 2);
    rounded = (m + 1) << (hb - 2);
    return static_cast<std::size_t>(hb - 11) * 4 + (m - 4);
}

Span* SizeClassHeap::carveSpan() {
    if (m_nextSpan == m_chunkEnd) {
        std::size_t raw = (kChunkSpans + 1) * kSpanSize;
        char* block = static_cast<char*>(osNew(raw));
        *reinterpret_cast<void**>(block) = m_chunks;
        m_chunks = block;
        m_chunkBytes += raw;
        ++m_chunkCount;
        auto first =
            (reinterpret_cast<std::uintptr_t>(block) + 16 + kSpanSize - 1) &
            ~(kSpanSize - 1);
        m_nextSpan = reinterpret_cast<char*>(first);
        m_chunkEnd = m_nextSpan + kChunkSpans * kSpanSize;
    }
    Span* s = reinterpret_cast<Span*>(m_nextSpan);
    m_nextSpan += kSpanSize;
    s->listed = false;
    return s;
}

Span* SizeClassHeap::takeSpan(Kind kind, std::size_t cls) {
    Span* s;
    if (m_pool != nullptr) {
        s = m_pool;
        m_pool = s->partialNext;
    } else {
        s = carveSpan();
    }
    const ClassTable& t = table();
    int buffer = kind == Kind::Buffer ? 1 : 0;
    s->partialNext = nullptr;
    s->partialPrev = nullptr;
    s->freeHead = nullptr;
    s->bump = 0;
    s->used = 0;
    s->slotSize = static_cast<std::uint32_t>(kClassSizes[cls]);
    s->slots = t.slots[buffer][cls];
    s->slotsOffset = t.slotsOffset[buffer][cls];
    s->reciprocal = t.reciprocal[cls];
    s->cls = static_cast<std::uint16_t>(cls);
    s->bitmapWords = static_cast<std::uint16_t>(t.words[buffer][cls]);
    s->kind = static_cast<std::uint8_t>(kind);
    s->inPartial = false;
    if (kind == Kind::Object) {
        std::memset(s->marks(), 0, 2 * s->bitmapWords * sizeof(std::uint64_t));
        if (!s->listed) {
            s->listed = true;
            s->objectNext = m_objectSpans;
            m_objectSpans = s;
        }
    }
    return s;
}

void SizeClassHeap::linkPartial(Span* s) {
    SizeClass& c = (s->kind == static_cast<std::uint8_t>(Kind::Object)
                        ? m_objClasses
                        : m_bufClasses)[s->cls];
    s->partialPrev = nullptr;
    s->partialNext = c.partial;
    if (c.partial != nullptr) {
        c.partial->partialPrev = s;
    }
    c.partial = s;
    s->inPartial = true;
}

void SizeClassHeap::unlinkPartial(Span* s) {
    SizeClass& c = (s->kind == static_cast<std::uint8_t>(Kind::Object)
                        ? m_objClasses
                        : m_bufClasses)[s->cls];
    if (s->partialPrev != nullptr) {
        s->partialPrev->partialNext = s->partialNext;
    } else {
        c.partial = s->partialNext;
    }
    if (s->partialNext != nullptr) {
        s->partialNext->partialPrev = s->partialPrev;
    }
    s->partialNext = nullptr;
    s->partialPrev = nullptr;
    s->inPartial = false;
}

void* SizeClassHeap::popSlot(Span* s) {
    void* p;
    if (s->freeHead != nullptr) {
        p = s->freeHead;
        s->freeHead = *static_cast<void**>(p);
    } else {
        p = s->slot(s->bump++);
    }
    ++s->used;
    if (s->freeHead == nullptr && s->bump == s->slots) {
        unlinkPartial(s);
    }
    return p;
}

void* SizeClassHeap::allocSlow(Kind kind, std::size_t cls) {
    Span* s = takeSpan(kind, cls);
    linkPartial(s);
    return popSlot(s);
}

void SizeClassHeap::freeSlot(Span* s, void* p) {
    *static_cast<void**>(p) = s->freeHead;
    s->freeHead = p;
    --s->used;
    if (!s->inPartial) {
        linkPartial(s);
    }
}

void SizeClassHeap::releaseIfEmpty(Span* s) {
    if (s->used != 0 || !s->inPartial) {
        return;
    }
    // Keep the last span that has room, so that an allocation that follows a
    // free does not take a span from the pool each time.
    if (s->partialNext == nullptr && s->partialPrev == nullptr) {
        return;
    }
    unlinkPartial(s);
    s->kind = static_cast<std::uint8_t>(Kind::Pooled);
    s->partialNext = m_pool;
    m_pool = s;
}

void* SizeClassHeap::allocObject(std::size_t size) {
    std::size_t cls = smallClass(size);
    Span* s = m_objClasses[cls].partial;
    if (s != nullptr) [[likely]] {
        return popSlot(s);
    }
    return allocSlow(Kind::Object, cls);
}

void SizeClassHeap::commitObject(void* p) {
    Span* s = size_class_heap_detail::spanOf(p);
    std::uint32_t i = s->indexOf(p);
    s->allocs()[i >> 6] |= std::uint64_t{1} << (i & 63);
    ++m_objects;
}

void SizeClassHeap::freeObject(void* p, bool committed) {
    Span* s = size_class_heap_detail::spanOf(p);
    if (committed) {
        std::uint32_t i = s->indexOf(p);
        s->allocs()[i >> 6] &= ~(std::uint64_t{1} << (i & 63));
        --m_objects;
    }
    freeSlot(s, p);
    releaseIfEmpty(s);
}

void SizeClassHeap::destroyAllObjects() {
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
                Obj* obj = reinterpret_cast<Obj*>(s->slot(i));
                obj->~Obj();
                s->allocs()[w] &= ~(std::uint64_t{1} << (i & 63));
                --m_objects;
                freeSlot(s, obj);
            }
            s->marks()[w] = 0;
        }
        releaseIfEmpty(s);
    }
}

void* SizeClassHeap::allocBuffer(std::size_t bytes) {
    if (bytes <= kMaxSmall) [[likely]] {
        std::size_t cls = smallClass(bytes);
        Span* s = m_bufClasses[cls].partial;
        if (s != nullptr) [[likely]] {
            return popSlot(s);
        }
        return allocSlow(Kind::Buffer, cls);
    }
    return allocLarge(bytes);
}

void SizeClassHeap::freeBuffer(void* p, std::size_t bytes) noexcept {
    if (bytes <= kMaxSmall) [[likely]] {
        Span* s = size_class_heap_detail::spanOf(p);
        freeSlot(s, p);
        releaseIfEmpty(s);
        return;
    }
    freeLarge(p, bytes);
}

void* SizeClassHeap::allocLarge(std::size_t bytes) {
    std::size_t rounded;
    std::size_t cls = largeClass(bytes, rounded);
    void* p = m_largeFree[cls];
    if (p != nullptr) {
        m_largeFree[cls] = *static_cast<void**>(p);
        m_largeCachedBytes -= rounded;
    } else {
        p = osNew(rounded);
    }
    m_largeLiveBytes += rounded;
    return p;
}

void SizeClassHeap::freeLarge(void* p, std::size_t bytes) noexcept {
    std::size_t rounded;
    std::size_t cls = largeClass(bytes, rounded);
    m_largeLiveBytes -= rounded;
    if (m_largeCachedBytes + rounded <=
        std::max(m_largeLiveBytes, kLargeCacheFloor)) {
        *static_cast<void**>(p) = m_largeFree[cls];
        m_largeFree[cls] = p;
        m_largeCachedBytes += rounded;
        return;
    }
    osDelete(p);
}
