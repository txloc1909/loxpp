#include "foreign_set.h"

#include <gtest/gtest.h>
#include <set>

namespace {

// The set never reads through a key, so a number that is a multiple of 16 is
// a key as good as an object.
Obj* key(std::size_t i) { return reinterpret_cast<Obj*>((i + 1) * 16); }

// Adds keys until a rehash is running. Returns the next key number.
std::size_t fillUntilRehash(ForeignSet& set, std::size_t from = 0) {
    std::size_t i = from;
    while (set.rehashIndex() < 0) {
        EXPECT_TRUE(set.touch(key(i++)));
    }
    return i;
}

} // namespace

TEST(ForeignSetTest, ATouchAddsOnceAndMarks) {
    ForeignSet set;
    EXPECT_TRUE(set.touch(key(1)));
    EXPECT_FALSE(set.touch(key(1)));
    EXPECT_EQ(set.size(), 1u);
    EXPECT_TRUE(set.marked(key(1)));
    EXPECT_FALSE(set.marked(key(2)));
}

TEST(ForeignSetTest, AnEntryThatIsNotTouchedIsNotMarkedInTheNextCycle) {
    ForeignSet set;
    set.touch(key(1));
    set.touch(key(2));
    set.beginCycle();
    EXPECT_FALSE(set.marked(key(1)));
    EXPECT_FALSE(set.marked(key(2)));
    EXPECT_FALSE(set.touch(key(1)));
    EXPECT_TRUE(set.marked(key(1)));
    EXPECT_FALSE(set.marked(key(2)));
    set.beginCycle();
    EXPECT_FALSE(set.marked(key(1)));
}

TEST(ForeignSetTest, ALookupDuringARehashFindsEveryEntry) {
    ForeignSet set;
    std::size_t n = fillUntilRehash(set, 0);
    // Entries that were added before the rehash and entries that go to the
    // new table: both must be found, and none is added a second time.
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_TRUE(set.marked(key(i))) << i;
    }
    for (std::size_t i = n; i < n + 8 && set.rehashIndex() >= 0; ++i) {
        EXPECT_TRUE(set.touch(key(i)));
        EXPECT_TRUE(set.marked(key(i)));
    }
    const std::size_t before = set.size();
    for (std::size_t i = 0; i < before; ++i) {
        if (set.rehashIndex() < 0) {
            break;
        }
        EXPECT_FALSE(set.touch(key(i))) << i;
    }
    EXPECT_EQ(set.size(), before);
}

TEST(ForeignSetTest, ATouchMovesAFewBucketsOfARehash) {
    ForeignSet set;
    // A table that is large enough for the old table to have many buckets.
    std::size_t n = 0;
    for (; n < 5000; ++n) {
        set.touch(key(n));
    }
    n = fillUntilRehash(set, n);
    long idx = set.rehashIndex();
    ASSERT_GE(idx, 0);
    int steps = 0;
    while (set.rehashIndex() >= 0 && steps < 100000) {
        const long before = set.rehashIndex();
        set.touch(key(n++));
        const long after = set.rehashIndex();
        if (after >= 0) {
            // At most kRehashBuckets chains moved and kRehashEmpty empty
            // buckets passed; a chain that moves counts one bucket too.
            // The numbers are written out, so a change of the constants must
            // change this test too.
            EXPECT_LE(after - before, 4 + 32);
        }
        ++steps;
    }
    EXPECT_LT(set.rehashIndex(), 0);
    EXPECT_GT(steps, 100);
}

TEST(ForeignSetTest, AScanDropsTheUnmarkedEntriesDuringARehash) {
    ForeignSet set;
    std::size_t n = fillUntilRehash(set, 0);
    set.beginCycle();
    // Mark the even keys. The odd ones must go.
    for (std::size_t i = 0; i < n; i += 2) {
        set.touch(key(i));
    }
    set.beginScan();
    std::set<Obj*> dropped;
    std::size_t extra = n;
    while (set.scanning()) {
        set.scanStep(3, [&](Obj* o) { dropped.insert(o); });
        // The program adds entries between the parts of the scan, and the
        // rehash goes on. A new entry is marked, so the scan never drops it.
        set.touch(key(extra++));
    }
    std::set<Obj*> want;
    for (std::size_t i = 1; i < n; i += 2) {
        want.insert(key(i));
    }
    EXPECT_EQ(dropped, want);
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(set.marked(key(i)), i % 2 == 0) << i;
    }
    for (std::size_t i = n; i < extra; ++i) {
        EXPECT_TRUE(set.marked(key(i))) << i;
    }
    EXPECT_EQ(set.size(), (n + 1) / 2 + (extra - n));
}

TEST(ForeignSetTest, AnEntryThatIsMarkedDuringAScanStays) {
    ForeignSet set;
    for (std::size_t i = 0; i < 10; ++i) {
        set.touch(key(i));
    }
    set.beginCycle();
    set.beginScan();
    std::set<Obj*> dropped;
    set.scanStep(4, [&](Obj* o) { dropped.insert(o); });
    // Entries not yet visited: mark one of them now.
    set.touch(key(0));
    set.touch(key(1));
    while (set.scanning()) {
        set.scanStep(4, [&](Obj* o) { dropped.insert(o); });
    }
    EXPECT_EQ(dropped.count(key(0)) + dropped.count(key(1)), 0u);
    EXPECT_TRUE(set.marked(key(0)));
    EXPECT_TRUE(set.marked(key(1)));
    EXPECT_EQ(set.size(), 2u);
}
