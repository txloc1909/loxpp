#include "size_class_heap.h"

#include <algorithm>
#include <cstdlib>
#include <sys/mman.h>
#include <cstring>
#include <ctime>

// A freed slot or block cannot be read or written until it is handed out
// again. AddressSanitizer then reports a use after free of an object or of a
// buffer, as it does for a block that came from `new`.
#if defined(__SANITIZE_ADDRESS__)
#define LOXPP_HEAP_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LOXPP_HEAP_ASAN 1
#endif
#endif
#ifdef LOXPP_HEAP_ASAN
#include <sanitizer/asan_interface.h>
#endif

namespace {

constexpr std::size_t kClassSizes[24] = {
    16,  32,  48,  64,  80,  96,  112, 128,  160,  192,  224,  256,
    320, 384, 448, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048};

constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kPage = 4096;

std::size_t pageRound(std::size_t n) { return (n + kPage - 1) & ~(kPage - 1); }

#ifdef LOXPP_HEAP_ASAN
void asanPoison(void* p, std::size_t n) { ASAN_POISON_MEMORY_REGION(p, n); }
void asanUnpoison(void* p, std::size_t n) { ASAN_UNPOISON_MEMORY_REGION(p, n); }
#else
void asanPoison(void*, std::size_t) {}
void asanUnpoison(void*, std::size_t) {}
#endif
constexpr std::size_t kSpanBytes = SizeClassHeap::kSpanSize;

// Cached large blocks stay below the larger of the large bytes in use and
// this floor. A block over the limit waits to go back to the kernel, one
// slice of kReleaseSlice for each step (releaseStep).
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

// A node sits in the first bytes of the block that it describes. The block
// loses its tail slice by slice, so the first page stays until the last call.
struct SizeClassHeap::ReleaseNode {
    ReleaseNode* next;
    std::size_t remaining;
};

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
    // The heap ends: everything goes back, in as many calls as it takes.
    for (std::size_t cls = 0; cls < kLargeClasses; ++cls) {
        std::size_t hb = cls / 4 + 11;
        std::size_t m = cls % 4 + 4;
        std::size_t mapBytes = pageRound((m + 1) << (hb - 2));
        while (void* head = m_largeFree[cls]) {
            asanUnpoison(head, mapBytes);
            m_largeFree[cls] = *static_cast<void**>(head);
            munmap(head, mapBytes);
        }
    }
    while (m_pending != nullptr) {
        ReleaseNode* node = m_pending;
        m_pending = node->next;
        munmap(node, node->remaining);
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

void* SizeClassHeap::osMap(std::size_t bytes) {
    ++m_osCalls;
    std::uint64_t t0 = m_stallTrace != nullptr ? nowNs() : 0;
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        throw std::bad_alloc();
    }
    if (m_stallTrace != nullptr) {
        noteStall("new", t0, nowNs());
    }
    return p;
}

void SizeClassHeap::osUnmap(void* p, std::size_t bytes) noexcept {
    ++m_osCalls;
    m_largestRelease = std::max(m_largestRelease, bytes);
    std::uint64_t t0 = m_stallTrace != nullptr ? nowNs() : 0;
    asanUnpoison(p, bytes);
    munmap(p, bytes);
    if (m_stallTrace != nullptr) {
        noteStall("free", t0, nowNs());
    }
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
        releaseBytes(raw);
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
    // The span may have held another class: its slots were poisoned, and the
    // header and the bitmaps of this class lie over them.
    asanUnpoison(s, kSpanBytes);
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
    asanPoison(s->slot(0), static_cast<std::size_t>(s->slots) * s->slotSize);
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
        asanUnpoison(p, s->slotSize);
        s->freeHead = *static_cast<void**>(p);
    } else {
        p = s->slot(s->bump++);
        asanUnpoison(p, s->slotSize);
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
    asanPoison(p, s->slotSize);
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

void SizeClassHeap::releaseBytes(std::size_t bytes) {
    while (bytes > 0) {
        std::size_t released = releaseStep(true);
        if (released == 0) {
            return;
        }
        bytes -= std::min(bytes, released);
    }
}

void* SizeClassHeap::allocLarge(std::size_t bytes) {
    std::size_t rounded;
    std::size_t cls = largeClass(bytes, rounded);
    std::size_t mapBytes = pageRound(rounded);
    void* p = m_largeFree[cls];
    if (p != nullptr) {
        asanUnpoison(p, mapBytes);
        m_largeFree[cls] = *static_cast<void**>(p);
        m_largeCachedBytes -= mapBytes;
    } else {
        // New memory: give back as much from the queue first.
        releaseBytes(mapBytes);
        p = osMap(mapBytes);
    }
    m_largeLiveBytes += mapBytes;
    releaseStep();
    return p;
}

void SizeClassHeap::freeLarge(void* p, std::size_t bytes) noexcept {
    std::size_t rounded;
    std::size_t cls = largeClass(bytes, rounded);
    std::size_t mapBytes = pageRound(rounded);
    m_largeLiveBytes -= mapBytes;
    if (m_largeCachedBytes + mapBytes <=
        std::max(m_largeLiveBytes, kLargeCacheFloor)) {
        *static_cast<void**>(p) = m_largeFree[cls];
        m_largeFree[cls] = p;
        m_largeCachedBytes += mapBytes;
        asanPoison(p, mapBytes);
    } else {
        queueRelease(p, mapBytes);
    }
    releaseStep();
}

void SizeClassHeap::queueRelease(void* p, std::size_t bytes) noexcept {
    asanUnpoison(p, bytes);
    auto* node = static_cast<ReleaseNode*>(p);
    node->next = m_pending;
    node->remaining = bytes;
    m_pending = node;
    m_largePendingBytes += bytes;
}

std::size_t SizeClassHeap::releaseStep(bool evictCache) {
    if (m_pending == nullptr) {
        // Trim the cache when the live large bytes have fallen under it. One
        // block for each step, the largest class first: the scan is over a
        // fixed number of classes.
        if (evictCache ||
            m_largeCachedBytes > std::max(m_largeLiveBytes, kLargeCacheFloor)) {
            for (std::size_t cls = kLargeClasses; cls-- > 0;) {
                void* p = m_largeFree[cls];
                if (p == nullptr) {
                    continue;
                }
                std::size_t rounded = 0;
                // Recover the size of the class from its index.
                std::size_t hb = cls / 4 + 11;
                std::size_t m = cls % 4 + 4;
                rounded = (m + 1) << (hb - 2);
                std::size_t mapBytes = pageRound(rounded);
                asanUnpoison(p, mapBytes);
                m_largeFree[cls] = *static_cast<void**>(p);
                m_largeCachedBytes -= mapBytes;
                queueRelease(p, mapBytes);
                break;
            }
        }
        if (m_pending == nullptr) {
            return 0;
        }
    }
    ReleaseNode* node = m_pending;
    if (node->remaining <= kReleaseSlice) {
        std::size_t bytes = node->remaining;
        m_pending = node->next;
        m_largePendingBytes -= bytes;
        osUnmap(node, bytes);
        return bytes;
    }
    node->remaining -= kReleaseSlice;
    m_largePendingBytes -= kReleaseSlice;
    osUnmap(reinterpret_cast<char*>(node) + node->remaining, kReleaseSlice);
    return kReleaseSlice;
}
