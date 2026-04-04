#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "object.h"
#include "simple_allocator.h"
#include "table.h"
#include "value.h"

// Fake hash for tests: returns the first byte of the string (0 for empty).
static uint32_t fakeHash(const char* key, int length) {
    if (length == 0)
        return 0u;
    return static_cast<uint32_t>(static_cast<unsigned char>(key[0]));
}

static ObjString* mkstr(SimpleAllocator& alloc, const std::string& s) {
    ObjHandle h = alloc.makeString(s);
    auto* obj = static_cast<ObjString*>(alloc.deref(h));
    obj->hash = fakeHash(s.c_str(), static_cast<int>(s.size()));
    return obj;
}

class TableTest : public ::testing::Test {
  protected:
    SimpleAllocator allocator_;
    Table table;

    ObjString* str(const std::string& s) { return mkstr(allocator_, s); }

    std::pair<ObjString*, ObjString*> collidingPair() {
        return {str("aardvark"), str("antelope")};
    }

    std::vector<ObjString*> collidingN(int n) {
        std::vector<ObjString*> keys;
        for (int i = 0; i < n; ++i)
            keys.push_back(str("a" + std::to_string(i)));
        return keys;
    }
};

// ============================================================
// 1. ObjString hash field
// ============================================================

TEST(HashFieldTest, SameContentSameHash) {
    SimpleAllocator alloc;
    ObjString* a = mkstr(alloc, "hello");
    ObjString* b = mkstr(alloc, "hello");
    EXPECT_EQ(a->hash, b->hash);
}

TEST(HashFieldTest, EmptyStringNoCrash) {
    SimpleAllocator alloc;
    ObjString* obj = mkstr(alloc, "");
    EXPECT_EQ(obj->hash, 0u);
}

// ============================================================
// 2. Table construction / destruction
// ============================================================

TEST(TableLifecycleTest, DefaultConstructedTableIsEmpty) {
    Table t;
    EXPECT_EQ(t.findString("anything", 8, 12345u), nullptr);
}

TEST(TableLifecycleTest, DestructorNoCrashOnEmpty) {
    Table t;
}

TEST(TableLifecycleTest, DestructorNoCrashAfterInserts) {
    SimpleAllocator alloc;
    Table t;
    for (int i = 0; i < 20; ++i) {
        std::string s = "key" + std::to_string(i);
        ObjString* k = mkstr(alloc, s);
        t.set(k, Value{static_cast<double>(i)});
    }
}

// ============================================================
// 3. set — insertion
// ============================================================

TEST_F(TableTest, SetNewKeyReturnsTrue) {
    EXPECT_TRUE(table.set(str("foo"), Value{1.0}));
}

TEST_F(TableTest, SetSecondDistinctKeyReturnsTrue) {
    table.set(str("foo"), Value{1.0});
    EXPECT_TRUE(table.set(str("bar"), Value{2.0}));
}

TEST_F(TableTest, OverwriteExistingKeyReturnsFalse) {
    ObjString* key = str("foo");
    table.set(key, Value{1.0});
    EXPECT_FALSE(table.set(key, Value{2.0}));
}

TEST_F(TableTest, OverwriteUpdatesStoredValue) {
    ObjString* key = str("foo");
    table.set(key, Value{1.0});
    table.set(key, Value{99.0});
    Value out;
    table.get(key, out);
    EXPECT_DOUBLE_EQ(as<double>(out), 99.0);
}

TEST_F(TableTest, SetNilValue) {
    ObjString* key = str("nil_key");
    table.set(key, Value{std::monostate{}});
    Value out;
    EXPECT_TRUE(table.get(key, out));
    EXPECT_TRUE(is<Nil>(out));
}

TEST_F(TableTest, SetBoolValue) {
    ObjString* key = str("flag");
    table.set(key, Value{true});
    Value out;
    EXPECT_TRUE(table.get(key, out));
    EXPECT_EQ(as<bool>(out), true);
}

TEST_F(TableTest, SetNumberValue) {
    ObjString* key = str("num");
    table.set(key, Value{3.14});
    Value out;
    EXPECT_TRUE(table.get(key, out));
    EXPECT_DOUBLE_EQ(as<double>(out), 3.14);
}

TEST_F(TableTest, SetObjHandleValue) {
    ObjString* key = str("objkey");
    ObjHandle valHandle = allocator_.makeString("objval");
    table.set(key, Value{valHandle});
    Value out;
    EXPECT_TRUE(table.get(key, out));
    EXPECT_EQ(as<ObjHandle>(out), valHandle);
}

// ============================================================
// 4. get — lookup
// ============================================================

TEST_F(TableTest, GetFromEmptyReturnsFalse) {
    Value out;
    EXPECT_FALSE(table.get(str("missing"), out));
}

TEST_F(TableTest, GetMissingKeyReturnsFalse) {
    table.set(str("foo"), Value{1.0});
    Value out;
    EXPECT_FALSE(table.get(str("bar"), out));
}

TEST_F(TableTest, GetExistingKeyReturnsTrue) {
    ObjString* key = str("key");
    table.set(key, Value{42.0});
    Value out;
    EXPECT_TRUE(table.get(key, out));
    EXPECT_DOUBLE_EQ(as<double>(out), 42.0);
}

TEST_F(TableTest, GetAfterOverwriteReturnsNewValue) {
    ObjString* key = str("key");
    table.set(key, Value{1.0});
    table.set(key, Value{2.0});
    Value out;
    table.get(key, out);
    EXPECT_DOUBLE_EQ(as<double>(out), 2.0);
}

TEST_F(TableTest, GetOutputParamUnchangedOnMiss) {
    Value out = Value{99.0};
    table.get(str("missing"), out);
    EXPECT_DOUBLE_EQ(as<double>(out), 99.0);
}

// ============================================================
// 5. del — observable removal
// ============================================================

TEST_F(TableTest, DeleteFromEmptyReturnsFalse) {
    EXPECT_FALSE(table.del(str("missing")));
}

TEST_F(TableTest, DeleteMissingKeyReturnsFalse) {
    table.set(str("foo"), Value{1.0});
    EXPECT_FALSE(table.del(str("bar")));
}

TEST_F(TableTest, DeleteExistingKeyReturnsTrue) {
    ObjString* key = str("foo");
    table.set(key, Value{1.0});
    EXPECT_TRUE(table.del(key));
}

TEST_F(TableTest, DeletedKeyIsNoLongerFound) {
    ObjString* key = str("foo");
    table.set(key, Value{1.0});
    table.del(key);
    Value out;
    EXPECT_FALSE(table.get(key, out));
}

TEST_F(TableTest, DeleteDoesNotOrphanCollidingKey) {
    auto [a, b] = collidingPair();
    table.set(a, Value{1.0});
    table.set(b, Value{2.0});
    table.del(a);
    Value out;
    EXPECT_TRUE(table.get(b, out));
    EXPECT_DOUBLE_EQ(as<double>(out), 2.0);
}

TEST_F(TableTest, DeleteThenReinsertSameKey) {
    ObjString* key = str("foo");
    table.set(key, Value{1.0});
    table.del(key);
    EXPECT_TRUE(table.set(key, Value{3.0}));
    Value out;
    EXPECT_TRUE(table.get(key, out));
    EXPECT_DOUBLE_EQ(as<double>(out), 3.0);
}

TEST_F(TableTest, DeleteSeveralFromCollisionChainRemainingStillFound) {
    auto keys = collidingN(3);
    for (size_t i = 0; i < keys.size(); ++i) {
        table.set(keys[i], Value{static_cast<double>(i)});
    }
    table.del(keys[0]);
    table.del(keys[1]);
    Value out;
    EXPECT_TRUE(table.get(keys[2], out));
    EXPECT_DOUBLE_EQ(as<double>(out), 2.0);
}

// ============================================================
// 6. Probing / collision correctness
// ============================================================

TEST_F(TableTest, TwoCollidingKeysBothFindable) {
    auto [a, b] = collidingPair();
    table.set(a, Value{10.0});
    table.set(b, Value{20.0});
    Value va, vb;
    EXPECT_TRUE(table.get(a, va));
    EXPECT_TRUE(table.get(b, vb));
    EXPECT_DOUBLE_EQ(as<double>(va), 10.0);
    EXPECT_DOUBLE_EQ(as<double>(vb), 20.0);
}

TEST_F(TableTest, ThreeCollidingKeysAllFindable) {
    auto keys = collidingN(3);
    for (size_t i = 0; i < keys.size(); ++i) {
        table.set(keys[i], Value{static_cast<double>(i)});
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        Value out;
        EXPECT_TRUE(table.get(keys[i], out)) << "key[" << i << "] missing";
        EXPECT_DOUBLE_EQ(as<double>(out), static_cast<double>(i));
    }
}

TEST_F(TableTest, MixedCollidingAndNonCollidingKeys) {
    auto [a, b] = collidingPair();
    ObjString* c = str("unique_xyz_key");
    table.set(a, Value{10.0});
    table.set(b, Value{20.0});
    table.set(c, Value{30.0});
    Value va, vb, vc;
    EXPECT_TRUE(table.get(a, va));
    EXPECT_DOUBLE_EQ(as<double>(va), 10.0);
    EXPECT_TRUE(table.get(b, vb));
    EXPECT_DOUBLE_EQ(as<double>(vb), 20.0);
    EXPECT_TRUE(table.get(c, vc));
    EXPECT_DOUBLE_EQ(as<double>(vc), 30.0);
}

TEST_F(TableTest, StressManyInsertsAllRetrievable) {
    const int N = 200;
    std::vector<ObjString*> keys;
    for (int i = 0; i < N; ++i) {
        keys.push_back(str("stress" + std::to_string(i)));
        table.set(keys.back(), Value{static_cast<double>(i)});
    }
    for (int i = 0; i < N; ++i) {
        Value out;
        EXPECT_TRUE(table.get(keys[i], out)) << "stress" << i << " missing";
        EXPECT_DOUBLE_EQ(as<double>(out), static_cast<double>(i));
    }
}

// ============================================================
// 7. addAll
// ============================================================

TEST_F(TableTest, AddAllEmptySourceLeavesDestUnchanged) {
    Table src;
    table.addAll(src);
    Value out;
    EXPECT_FALSE(table.get(str("any"), out));
}

TEST_F(TableTest, AddAllCopiesEntriesToEmptyDest) {
    SimpleAllocator srcAlloc;
    Table src;
    ObjString* k1 = mkstr(srcAlloc, "alpha");
    ObjString* k2 = mkstr(srcAlloc, "beta");
    src.set(k1, Value{1.0});
    src.set(k2, Value{2.0});

    table.addAll(src);

    Value v1, v2;
    EXPECT_TRUE(table.get(k1, v1));
    EXPECT_DOUBLE_EQ(as<double>(v1), 1.0);
    EXPECT_TRUE(table.get(k2, v2));
    EXPECT_DOUBLE_EQ(as<double>(v2), 2.0);
}

TEST_F(TableTest, AddAllNoOverlapUnionInDest) {
    SimpleAllocator srcAlloc;
    Table src;

    ObjString* destKey = str("dest_only");
    table.set(destKey, Value{9.0});

    ObjString* srcKey = mkstr(srcAlloc, "src_only");
    src.set(srcKey, Value{7.0});

    table.addAll(src);

    Value vd, vs;
    EXPECT_TRUE(table.get(destKey, vd));
    EXPECT_DOUBLE_EQ(as<double>(vd), 9.0);
    EXPECT_TRUE(table.get(srcKey, vs));
    EXPECT_DOUBLE_EQ(as<double>(vs), 7.0);
}

TEST_F(TableTest, AddAllOverlappingKeysTakesSourceValue) {
    Table src;
    ObjString* sharedKey = str("shared");
    table.set(sharedKey, Value{1.0});
    src.set(sharedKey, Value{99.0});

    table.addAll(src);

    Value out;
    table.get(sharedKey, out);
    EXPECT_DOUBLE_EQ(as<double>(out), 99.0);
}

TEST_F(TableTest, AddAllDoesNotMutateSource) {
    SimpleAllocator srcAlloc;
    Table src;
    ObjString* k = mkstr(srcAlloc, "orig");
    src.set(k, Value{5.0});

    table.addAll(src);

    Value out;
    EXPECT_TRUE(src.get(k, out));
    EXPECT_DOUBLE_EQ(as<double>(out), 5.0);
}

// ============================================================
// 8. findString — raw character lookup
// ============================================================

TEST_F(TableTest, FindStringInEmptyTableReturnsNull) {
    uint32_t h = fakeHash("hello", 5);
    EXPECT_EQ(table.findString("hello", 5, h), nullptr);
}

TEST_F(TableTest, FindStringAbsentReturnsNull) {
    ObjString* k = str("present");
    table.set(k, Value{std::monostate{}});
    uint32_t h = fakeHash("absent", 6);
    EXPECT_EQ(table.findString("absent", 6, h), nullptr);
}

TEST_F(TableTest, FindStringPresentReturnsPointer) {
    ObjString* k = str("hello");
    table.set(k, Value{std::monostate{}});
    uint32_t h = fakeHash("hello", 5);
    EXPECT_EQ(table.findString("hello", 5, h), k);
}

TEST_F(TableTest, FindStringHashCollisionDifferentContentReturnsNull) {
    auto [a, b] = collidingPair();
    table.set(a, Value{std::monostate{}});
    EXPECT_EQ(table.findString(b->chars.c_str(),
                               static_cast<int>(b->chars.size()), b->hash),
              nullptr);
}

TEST_F(TableTest, FindStringByRawCharsWithoutObjStringWrapper) {
    ObjString* k = str("lookup_me");
    table.set(k, Value{std::monostate{}});
    const char* raw = "lookup_me";
    int len = static_cast<int>(k->chars.size());
    uint32_t h = fakeHash(raw, len);
    EXPECT_EQ(table.findString(raw, len, h), k);
}

// ============================================================
// 9. String interning (using Table as intern table directly)
// ============================================================

class StringInternTest : public ::testing::Test {
  protected:
    SimpleAllocator allocator_;
    Table internTable;

    ObjString* internString(const std::string& s) {
        uint32_t h = fakeHash(s.c_str(), static_cast<int>(s.size()));
        ObjString* found =
            internTable.findString(s.c_str(), static_cast<int>(s.size()), h);
        if (found)
            return found;
        ObjString* obj = mkstr(allocator_, s);
        internTable.set(obj, Value{std::monostate{}});
        return obj;
    }
};

TEST_F(StringInternTest, SameContentReturnsSamePointer) {
    ObjString* a = internString("hello");
    ObjString* b = internString("hello");
    EXPECT_EQ(a, b);
}

TEST_F(StringInternTest, DifferentContentReturnsDifferentPointers) {
    ObjString* a = internString("hello");
    ObjString* b = internString("world");
    EXPECT_NE(a, b);
}

TEST_F(StringInternTest, EmptyStringInternedConsistently) {
    ObjString* a = internString("");
    ObjString* b = internString("");
    EXPECT_EQ(a, b);
}

TEST_F(StringInternTest, InternedPointerEqualityHolds) {
    ObjString* a = internString("test");
    ObjString* b = internString("test");
    EXPECT_EQ(a, b);
    ObjString* c = internString("other");
    EXPECT_NE(a, c);
}

TEST_F(StringInternTest, ManyStringsAllInternable) {
    const int N = 100;
    std::vector<ObjString*> ptrs;
    for (int i = 0; i < N; ++i) {
        ptrs.push_back(internString("str" + std::to_string(i)));
    }
    for (int i = 0; i < N; ++i) {
        ObjString* p = internString("str" + std::to_string(i));
        EXPECT_EQ(p, ptrs[i]) << "Mismatch at i=" << i;
    }
}

TEST_F(StringInternTest, SingleCharStringsAllInternable) {
    std::vector<ObjString*> ptrs;
    for (char c = 'a'; c <= 'z'; ++c) {
        ptrs.push_back(internString(std::string(1, c)));
    }
    for (char c = 'a'; c <= 'z'; ++c) {
        ObjString* p = internString(std::string(1, c));
        EXPECT_EQ(p, ptrs[c - 'a']) << "Mismatch for '" << c << "'";
    }
}
