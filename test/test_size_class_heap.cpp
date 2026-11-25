#include "container_objects.h"
#include "memory_manager.h"
#include "size_class_heap.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>

// The layout of the heap: size classes, the mark bitmaps, a footprint that
// stays bounded under churn, and the calls into the C allocator
// (docs/decisions/garbage-collectors.md, phase 8).

// The calls of operator new and operator delete that happen inside a window
// that a test opens. A std:: container that an object owns calls them, and
// the heap does not time those calls (docs/reference/gc-runtime-contract.md,
// section 1).
namespace {
std::atomic<bool> g_countCalls{false};
std::atomic<std::size_t> g_news{0};
std::atomic<std::size_t> g_deletes{0};
} // namespace

// Every form of operator new and operator delete is replaced, so that a
// sanitizer sees a matching pair.
static void* countedNew(std::size_t n) {
    if (g_countCalls.load(std::memory_order_relaxed)) {
        g_news.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(n != 0 ? n : 1);
}
static void countedDelete(void* p) noexcept {
    if (p != nullptr && g_countCalls.load(std::memory_order_relaxed)) {
        g_deletes.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}
void* operator new(std::size_t n) {
    void* p = countedNew(n);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    return countedNew(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    return countedNew(n);
}
void operator delete(void* p) noexcept { countedDelete(p); }
void operator delete[](void* p) noexcept { countedDelete(p); }
void operator delete(void* p, std::size_t) noexcept { countedDelete(p); }
void operator delete[](void* p, std::size_t) noexcept { countedDelete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept {
    countedDelete(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    countedDelete(p);
}

namespace {

// A deterministic mix of sizes: small blocks of each class, a few near the
// limit of the classes, and a few large blocks.
std::size_t mixedSize(std::size_t i) {
    static const std::size_t sizes[] = {8,    24,   40,   100,   300,    1000,
                                        2048, 2049, 5000, 70000, 300000, 17};
    return sizes[i % (sizeof(sizes) / sizeof(sizes[0]))];
}

} // namespace

TEST(SizeClassHeap, AMarkBitBelongsToOneObject) {
    MemoryManager mm;
    SizeClassHeap& store = mm.store();
    std::vector<ObjString*> strings;
    for (int i = 0; i < 300; ++i) {
        strings.push_back(mm.makeString("string number " + std::to_string(i)));
        mm.pushTempRoot(strings.back());
    }
    for (ObjString* s : strings) {
        EXPECT_FALSE(store.isMarked(s));
    }
    // Mark every third object: no neighbour in the span changes.
    for (std::size_t i = 0; i < strings.size(); i += 3) {
        EXPECT_TRUE(store.tryMark(strings[i]));
        EXPECT_FALSE(store.tryMark(strings[i]));
    }
    for (std::size_t i = 0; i < strings.size(); ++i) {
        EXPECT_EQ(store.isMarked(strings[i]), i % 3 == 0) << i;
    }
    // A sweep that kills nothing clears the marks.
    std::size_t live = 0;
    for (auto* s = store.firstObjectSpan(); s != nullptr;
         s = SizeClassHeap::nextObjectSpan(s)) {
        live += store.sweepSpan(s, [](Obj*) {});
    }
    (void)live;
    for (ObjString* s : strings) {
        EXPECT_FALSE(store.isMarked(s));
    }
}

TEST(SizeClassHeap, ASweepFreesTheUnmarkedAndKeepsTheMarked) {
    MemoryManager mm;
    SizeClassHeap& store = mm.store();
    std::vector<ObjString*> strings;
    for (int i = 0; i < 500; ++i) {
        strings.push_back(mm.makeString("kept or freed " + std::to_string(i)));
        mm.pushTempRoot(strings.back());
    }
    // Take the strings out of the intern table with the objects: the test
    // sweeps by hand, so it stops being a collector. A second collector run
    // would find a stale entry otherwise.
    for (std::size_t i = 0; i < strings.size(); i += 2) {
        store.tryMark(strings[i]);
    }
    std::size_t before = store.objectCount();
    std::size_t dead = 0;
    std::size_t live = 0;
    for (auto* s = store.firstObjectSpan(); s != nullptr;
         s = SizeClassHeap::nextObjectSpan(s)) {
        live += store.sweepSpan(s, [&](Obj*) { ++dead; });
    }
    EXPECT_EQ(live, strings.size() / 2);
    EXPECT_EQ(dead, before - live);
    EXPECT_EQ(store.objectCount(), live);
    // The intern table still points at the freed strings. Drop them with the
    // heap's own end: collectAll never reads them.
    for (std::size_t i = 0; i < strings.size(); i += 2) {
        EXPECT_EQ(std::string(strings[i]->chars.data(), 4), "kept");
    }
    while (!strings.empty()) {
        mm.popTempRoot();
        strings.pop_back();
    }
}

TEST(SizeClassHeap, ObjectsKeepTheirAddress) {
    MemoryManager mm;
    std::vector<ObjString*> strings;
    std::vector<const void*> addresses;
    for (int i = 0; i < 1000; ++i) {
        strings.push_back(mm.makeString("stable " + std::to_string(i)));
        mm.pushTempRoot(strings.back());
        addresses.push_back(strings.back());
    }
    for (int i = 0; i < 20; ++i) {
        mm.collectGarbage();
    }
    for (std::size_t i = 0; i < strings.size(); ++i) {
        EXPECT_EQ(static_cast<const void*>(strings[i]), addresses[i]);
        EXPECT_EQ(
            std::string(strings[i]->chars.data(), strings[i]->chars.size()),
            "stable " + std::to_string(i));
    }
    for (std::size_t i = 0; i < strings.size(); ++i) {
        mm.popTempRoot();
    }
}

TEST(SizeClassHeap, MixedSizeChurnKeepsTheFootprintBounded) {
    MemoryManager mm;
    SizeClassHeap& store = mm.store();
    constexpr int kPerRound = 200;
    std::vector<std::pair<void*, std::size_t>> previous, current;
    std::size_t footprintAtWarm = 0;
    std::size_t osCallsAtWarm = 0;
    std::size_t peakLive = 0;
    for (int round = 0; round < 300; ++round) {
        current.clear();
        std::size_t live = 0;
        for (int i = 0; i < kPerRound; ++i) {
            std::size_t n = mixedSize(static_cast<std::size_t>(i));
            current.push_back({mm.rawAlloc(n), n});
            live += n;
        }
        for (auto& [p, n] : previous) {
            mm.rawFree(p, n);
        }
        previous.swap(current);
        peakLive = std::max(peakLive, live * 2);
        if (round == 40) {
            footprintAtWarm = store.footprintBytes();
            osCallsAtWarm = store.osCalls();
        }
    }
    // After the warm-up, the same mix again and again must not ask the C
    // allocator for more, and must not grow the heap.
    EXPECT_EQ(store.osCalls(), osCallsAtWarm);
    EXPECT_EQ(store.footprintBytes(), footprintAtWarm);
    // Goal 3 of the decision: at most 2.5 times the live bytes.
    EXPECT_LE(static_cast<double>(store.footprintBytes()),
              2.5 * static_cast<double>(peakLive));
    for (auto& [p, n] : previous) {
        mm.rawFree(p, n);
    }
}

TEST(SizeClassHeap, ASpanThatEmptiesServesAnotherClass) {
    MemoryManager mm;
    SizeClassHeap& store = mm.store();
    constexpr int kBlocks = 200000;
    std::vector<void*> blocks;
    blocks.reserve(kBlocks);
    for (int i = 0; i < kBlocks; ++i) {
        blocks.push_back(mm.rawAlloc(48));
    }
    std::size_t chunks = store.chunkCount();
    for (void* p : blocks) {
        mm.rawFree(p, 48);
    }
    blocks.clear();
    // The same bytes in blocks of another class take the emptied spans, not
    // new chunks.
    for (int i = 0; i < kBlocks / 2; ++i) {
        blocks.push_back(mm.rawAlloc(96));
    }
    EXPECT_EQ(store.chunkCount(), chunks);
    for (void* p : blocks) {
        mm.rawFree(p, 96);
    }
}

TEST(SizeClassHeap, ASpanThatHeldBuffersServesObjects) {
    // The bitmaps of an object span lie over the slots that the span held as a
    // buffer span. A sanitizer build poisons the freed slots, so the span must
    // be unpoisoned when it takes its new class.
    MemoryManager mm;
    std::vector<void*> blocks;
    for (int i = 0; i < 50000; ++i) {
        blocks.push_back(mm.rawAlloc(48));
    }
    for (void* p : blocks) {
        mm.rawFree(p, 48);
    }
    std::vector<ObjString*> strings;
    for (int i = 0; i < 3000; ++i) {
        strings.push_back(mm.makeString("object " + std::to_string(i)));
        mm.pushTempRoot(strings.back());
    }
    mm.collectGarbage();
    for (std::size_t i = 0; i < strings.size(); ++i) {
        EXPECT_EQ(
            std::string(strings[i]->chars.data(), strings[i]->chars.size()),
            "object " + std::to_string(i));
        mm.popTempRoot();
    }
}

TEST(SizeClassHeap, SmallAllocationAndFreeNeverReachTheCAllocator) {
    MemoryManager mm;
    SizeClassHeap& store = mm.store();
    // Take one span of each class first: the first block of a class needs one.
    for (std::size_t n = 16; n <= SizeClassHeap::kMaxSmall; n += 16) {
        mm.rawFree(mm.rawAlloc(n), n);
    }
    std::size_t calls = store.osCalls();
    for (int i = 0; i < 100000; ++i) {
        void* p = mm.rawAlloc(static_cast<std::size_t>(16 + (i % 120) * 16));
        mm.rawFree(p, static_cast<std::size_t>(16 + (i % 120) * 16));
    }
    EXPECT_EQ(store.osCalls(), calls);
}

TEST(SizeClassHeap, ALargeBlockComesBackFromTheCache) {
    MemoryManager mm;
    SizeClassHeap& store = mm.store();
    void* a = mm.rawAlloc(100000);
    std::size_t calls = store.osCalls();
    mm.rawFree(a, 100000);
    void* b = mm.rawAlloc(100000);
    EXPECT_EQ(b, a);
    EXPECT_EQ(store.osCalls(), calls);
    mm.rawFree(b, 100000);
}

TEST(SizeClassHeap, ABlockOverTheCacheLimitGoesBackToTheCAllocator) {
    MemoryManager mm;
    SizeClassHeap& store = mm.store();
    std::size_t base = store.footprintBytes();
    void* a = mm.rawAlloc(std::size_t{8} << 20);
    void* b = mm.rawAlloc(std::size_t{8} << 20);
    mm.rawFree(a, std::size_t{8} << 20); // cached: the other block is live
    std::size_t withCache = store.footprintBytes();
    mm.rawFree(b, std::size_t{8} << 20); // over the limit: freed
    EXPECT_LT(store.footprintBytes(), withCache);
    EXPECT_GE(store.footprintBytes(), base);
}

TEST(SizeClassHeap, StallRecordsAreWrittenForCallsOverTheLimit) {
    std::FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    {
        MemoryManager mm;
        // A limit of 0 records every call that reaches the C allocator.
        mm.store().setStallTrace(f, 0);
        void* a = mm.rawAlloc(std::size_t{8} << 20);
        void* b = mm.rawAlloc(std::size_t{8} << 20);
        mm.rawFree(a, std::size_t{8} << 20);
        mm.rawFree(b, std::size_t{8} << 20);
        // A small block that needs a new span needs a chunk once.
        void* s = mm.rawAlloc(64);
        mm.rawFree(s, 64);
        mm.store().setStallTrace(nullptr, 0);
    }
    std::rewind(f);
    int news = 0, frees = 0;
    char line[256];
    while (std::fgets(line, sizeof line, f) != nullptr) {
        unsigned long long t0 = 0, t1 = 0;
        char kind[8] = {};
        ASSERT_EQ(
            std::sscanf(line, "stall kind=%7s t0=%llu t1=%llu", kind, &t0, &t1),
            3)
            << line;
        EXPECT_GE(t1, t0);
        std::string k = kind;
        news += k == "new";
        frees += k == "free";
    }
    std::fclose(f);
    EXPECT_GE(news, 3); // two large blocks and a chunk
    EXPECT_GE(frees, 1);
}

TEST(SizeClassHeap, NoRecordIsWrittenUnderTheLimit) {
    std::FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    {
        MemoryManager mm;
        mm.store().setStallTrace(f, 60ull * 1000 * 1000 * 1000);
        void* a = mm.rawAlloc(std::size_t{8} << 20);
        mm.rawFree(a, std::size_t{8} << 20);
        mm.store().setStallTrace(nullptr, 0);
    }
    EXPECT_EQ(std::ftell(f), 0);
    std::fclose(f);
}

TEST(SizeClassHeap, ACollectionDuringAConstructorDoesNotFreeTheNewSlot) {
    // A constructor can allocate, and an allocation can collect. The object
    // under construction is not in the heap yet, so the sweep must skip it.
    // A sweep that freed the slot would hand it out again, and two live
    // objects would share an address. Stress mode (the _gc twin of this test)
    // collects at each allocation.
    MemoryManager mm;
    std::set<const void*> addresses;
    std::vector<std::pair<ObjString*, std::string>> strings;
    for (int i = 0; i < 300; ++i) {
        std::string text = std::to_string(i) + std::string(100, 'x');
        ObjString* s = mm.makeString(text);
        mm.pushTempRoot(s);
        strings.push_back({s, text});
        addresses.insert(s);
    }
    EXPECT_EQ(addresses.size(), strings.size());
    for (auto& [s, text] : strings) {
        EXPECT_EQ(std::string(s->chars.data(), s->chars.size()), text);
        mm.popTempRoot();
    }
}

TEST(SizeClassHeap, AFreedLargeBlockGoesBackInSlices) {
    MemoryManager mm;
    SizeClassHeap& store = mm.store();
    constexpr std::size_t kBlock = std::size_t{256} << 20;
    void* p = mm.rawAlloc(kBlock);
    std::size_t base = store.footprintBytes() - kBlock;
    std::size_t calls = store.osCalls();
    mm.rawFree(p, kBlock);
    // The free gave back at most one slice. The rest waits.
    EXPECT_LE(store.largestReleaseBytes(), SizeClassHeap::kReleaseSlice);
    EXPECT_GT(store.pendingReleaseBytes(), 0u);
    std::size_t steps = 0;
    while (store.pendingReleaseBytes() != 0) {
        store.releaseStep();
        ++steps;
        ASSERT_LT(steps, 1000u);
    }
    EXPECT_EQ(store.largestReleaseBytes(), SizeClassHeap::kReleaseSlice);
    EXPECT_GE(store.osCalls() - calls, kBlock / SizeClassHeap::kReleaseSlice);
    EXPECT_EQ(store.footprintBytes(), base);
}

TEST(SizeClassHeap, TheSweepGivesBackASliceForEachSpan) {
    MemoryManager mm;
    SizeClassHeap& store = mm.store();
    constexpr std::size_t kBlock = std::size_t{64} << 20;
    mm.rawFree(mm.rawAlloc(kBlock), kBlock);
    std::size_t pending = store.pendingReleaseBytes();
    ASSERT_GT(pending, 0u);
    // A collection sweeps every span of the heap. Make a few spans first.
    std::vector<ObjString*> strings;
    for (int i = 0; i < 2000; ++i) {
        strings.push_back(mm.makeString("a string " + std::to_string(i)));
        mm.pushTempRoot(strings.back());
    }
    mm.collectGarbage();
    EXPECT_LT(store.pendingReleaseBytes(), pending);
    EXPECT_LE(store.largestReleaseBytes(), SizeClassHeap::kReleaseSlice);
    for (std::size_t i = 0; i < strings.size(); ++i) {
        mm.popTempRoot();
    }
}

TEST(SizeClassHeap, TheCacheShrinksWhenTheLiveLargeBytesFall) {
    MemoryManager mm;
    SizeClassHeap& store = mm.store();
    constexpr std::size_t kBlock = std::size_t{8} << 20;
    std::size_t base = store.footprintBytes();
    void* a = mm.rawAlloc(kBlock);
    void* b = mm.rawAlloc(kBlock);
    void* c = mm.rawAlloc(kBlock);
    mm.rawFree(a, kBlock); // cached: the others are live
    mm.rawFree(b, kBlock);
    mm.rawFree(c, kBlock);
    // Nothing is live now, so the cached block is over the limit.
    for (int i = 0; i < 200 && store.footprintBytes() > base + (4u << 20);
         ++i) {
        store.releaseStep();
    }
    EXPECT_LE(store.footprintBytes(), base + (4u << 20));
}

TEST(SizeClassHeap, EveryCallToTheCAllocatorOfTheHeapIsTimed) {
    std::FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    {
        MemoryManager mm;
        SizeClassHeap& store = mm.store();
        store.setStallTrace(f, 0);
        // Warm up: one span of each size class.
        for (std::size_t n = 16; n <= SizeClassHeap::kMaxSmall; n += 16) {
            mm.rawFree(mm.rawAlloc(n), n);
        }
        std::fflush(f);
        std::fseek(f, 0, SEEK_END);
        long recordsBefore = std::ftell(f);
        std::size_t callsBefore = store.osCalls();
        g_news = 0;
        g_deletes = 0;
        g_countCalls = true;
        // The heap path: small buffers, and a large block that goes through
        // the map, the cache and the release.
        for (int i = 0; i < 20000; ++i) {
            std::size_t n = 16 + static_cast<std::size_t>(i % 128) * 16;
            mm.rawFree(mm.rawAlloc(n), n);
        }
        std::size_t big = std::size_t{6} << 20;
        mm.rawFree(mm.rawAlloc(big), big);
        while (store.pendingReleaseBytes() != 0) {
            store.releaseStep();
        }
        g_countCalls = false;
        std::fflush(f);
        // No call of operator new or operator delete: the heap does not use
        // them on this path, and every call that it makes is the map or the
        // release of a large block.
        EXPECT_EQ(g_news.load(), 0u);
        EXPECT_EQ(g_deletes.load(), 0u);
        std::size_t calls = store.osCalls() - callsBefore;
        EXPECT_GT(calls, 0u);
        store.setStallTrace(nullptr, 0);
        std::fseek(f, recordsBefore, SEEK_SET);
        std::size_t records = 0;
        char line[256];
        while (std::fgets(line, sizeof line, f) != nullptr) {
            ++records;
            EXPECT_TRUE(std::string(line).rfind("stall kind=", 0) == 0) << line;
        }
        // With the limit at 0, one record for each call.
        EXPECT_EQ(records, calls);
    }
    std::fclose(f);
}

TEST(SizeClassHeap, TheUntimedCallsAreTheOnesTheContractNames) {
    // docs/reference/gc-runtime-contract.md, section 1, lists the calls to the
    // C allocator that nothing times: the std:: containers that an object or
    // the collector owns. A closure with upvalues is one of them. This test
    // keeps the contract and the code in step: if the vector of a closure
    // moves to the heap, change this test and the contract together.
    MemoryManager mm;
    ObjFunction* fn = mm.create<ObjFunction>();
    mm.pushTempRoot(fn);
    fn->upvalueCount = 3;
    g_news = 0;
    g_countCalls = true;
    ObjClosure* closure = mm.create<ObjClosure>(fn);
    g_countCalls = false;
    EXPECT_NE(closure, nullptr);
    EXPECT_GT(g_news.load(), 0u);
    mm.popTempRoot();
}

#if defined(__SANITIZE_ADDRESS__) ||                                           \
    (defined(__has_feature) && __has_feature(address_sanitizer))
TEST(SizeClassHeapDeathTest, ReadingAFreedStringIsReported) {
    // A freed slot is poisoned, so AddressSanitizer reports a read of a freed
    // object. It reports "use-after-poison", not "heap-use-after-free".
    EXPECT_DEATH(
        {
            MemoryManager mm;
            ObjString* s = mm.makeString("a string that nothing keeps");
            mm.collectGarbage();
            volatile std::uint32_t h = s->hash;
            (void)h;
        },
        "AddressSanitizer");
}

TEST(SizeClassHeapDeathTest, ReadingAFreedBufferIsReported) {
    EXPECT_DEATH(
        {
            MemoryManager mm;
            volatile char* p = static_cast<char*>(mm.rawAlloc(100));
            mm.rawFree(const_cast<char*>(p), 100);
            char c = p[0];
            (void)c;
        },
        "AddressSanitizer");
}

TEST(SizeClassHeapDeathTest, ReadingACachedLargeBlockIsReported) {
    EXPECT_DEATH(
        {
            MemoryManager mm;
            volatile char* p = static_cast<char*>(mm.rawAlloc(100000));
            mm.rawFree(const_cast<char*>(p), 100000);
            char c = p[50000];
            (void)c;
        },
        "AddressSanitizer");
}
#endif
