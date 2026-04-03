#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "memory.h"
#include "object.h"
#include "table.h"
#include "value.h"

// Fake hash for tests: returns the first byte of the string (0 for empty).
// Using just one byte makes it trivial to engineer collisions — any two strings
// that start with the same character always land in the same preferred bucket,
// regardless of table capacity.
static uint32_t fakeHash(const char* key, int length) {
    if (length == 0) return 0u;
    return static_cast<uint32_t>(static_cast<unsigned char>(key[0]));
}

// ============================================================
// Shared fixture
// ============================================================

class TableTest : public ::testing::Test {
  protected:
    std::vector<std::unique_ptr<Obj>> objects;
    Table table;

    void SetUp() override { initTable(&table); }
    void TearDown() override { freeTable(&table); }

    ObjString* str(const std::string& s) {
        ObjString* obj = makeString(objects, s);
        obj->hash = fakeHash(s.c_str(), static_cast<int>(s.size()));
        return obj;
    }

    // Both strings start with 'a', so fakeHash returns 97 for both —
    // guaranteed collision regardless of table capacity.
    std::pair<ObjString*, ObjString*> collidingPair() {
        return {str("aardvark"), str("antelope")};
    }

    // N strings all starting with 'a' — all collide via fakeHash.
    std::vector<ObjString*> collidingN(int n) {
        std::vector<ObjString*> keys;
        for (int i = 0; i < n; ++i)
            keys.push_back(str("a" + std::to_string(i)));
        return keys;
    }
};

// ============================================================
// 1. initTable / freeTable
// ============================================================

TEST(TableLifecycleTest, InitIsEmpty) {
    Table t;
    initTable(&t);
    EXPECT_EQ(t.count, 0);
    EXPECT_EQ(t.capacity, 0);
    EXPECT_EQ(t.entries, nullptr);
    freeTable(&t);
}

TEST(TableLifecycleTest, FreeEmptyTableNoCrash) {
    Table t;
    initTable(&t);
    freeTable(&t);
}

TEST(TableLifecycleTest, FreeAfterInserts) {
    std::vector<std::unique_ptr<Obj>> objects;
    Table t;
    initTable(&t);
    for (int i = 0; i < 20; ++i) {
        std::string s = "key" + std::to_string(i);
        ObjString* k = makeString(objects, s);
        k->hash = fakeHash(s.c_str(), static_cast<int>(s.size()));
        tableSet(&t, k, Value{static_cast<double>(i)});
    }
    freeTable(&t); // ASAN should stay clean
}

// ============================================================
// 3. tableSet — insertion
// ============================================================

TEST_F(TableTest, SetNewKeyReturnsTrue) {
    EXPECT_TRUE(tableSet(&table, str("foo"), Value{1.0}));
}

TEST_F(TableTest, SetSecondDistinctKeyReturnsTrue) {
    tableSet(&table, str("foo"), Value{1.0});
    EXPECT_TRUE(tableSet(&table, str("bar"), Value{2.0}));
}

TEST_F(TableTest, OverwriteExistingKeyReturnsFalse) {
    ObjString* key = str("foo");
    tableSet(&table, key, Value{1.0});
    EXPECT_FALSE(tableSet(&table, key, Value{2.0}));
}

TEST_F(TableTest, OverwriteUpdatesStoredValue) {
    ObjString* key = str("foo");
    tableSet(&table, key, Value{1.0});
    tableSet(&table, key, Value{99.0});
    Value out;
    tableGet(&table, key, out);
    EXPECT_DOUBLE_EQ(as<double>(out), 99.0);
}

TEST_F(TableTest, SetNilValue) {
    ObjString* key = str("nil_key");
    tableSet(&table, key, Value{std::monostate{}});
    Value out;
    EXPECT_TRUE(tableGet(&table, key, out));
    EXPECT_TRUE(is<Nil>(out));
}

TEST_F(TableTest, SetBoolValue) {
    ObjString* key = str("flag");
    tableSet(&table, key, Value{true});
    Value out;
    EXPECT_TRUE(tableGet(&table, key, out));
    EXPECT_EQ(as<bool>(out), true);
}

TEST_F(TableTest, SetNumberValue) {
    ObjString* key = str("num");
    tableSet(&table, key, Value{3.14});
    Value out;
    EXPECT_TRUE(tableGet(&table, key, out));
    EXPECT_DOUBLE_EQ(as<double>(out), 3.14);
}

TEST_F(TableTest, SetObjPtrValue) {
    ObjString* key = str("objkey");
    ObjString* val = str("objval");
    tableSet(&table, key, Value{static_cast<Obj*>(val)});
    Value out;
    EXPECT_TRUE(tableGet(&table, key, out));
    EXPECT_EQ(as<Obj*>(out), static_cast<Obj*>(val));
}

// ============================================================
// 4. tableGet — lookup
// ============================================================

TEST_F(TableTest, GetFromEmptyReturnsFalse) {
    Value out;
    EXPECT_FALSE(tableGet(&table, str("missing"), out));
}

TEST_F(TableTest, GetMissingKeyReturnsFalse) {
    tableSet(&table, str("foo"), Value{1.0});
    Value out;
    EXPECT_FALSE(tableGet(&table, str("bar"), out));
}

TEST_F(TableTest, GetExistingKeyReturnsTrue) {
    ObjString* key = str("key");
    tableSet(&table, key, Value{42.0});
    Value out;
    EXPECT_TRUE(tableGet(&table, key, out));
    EXPECT_DOUBLE_EQ(as<double>(out), 42.0);
}

TEST_F(TableTest, GetAfterOverwriteReturnsNewValue) {
    ObjString* key = str("key");
    tableSet(&table, key, Value{1.0});
    tableSet(&table, key, Value{2.0});
    Value out;
    tableGet(&table, key, out);
    EXPECT_DOUBLE_EQ(as<double>(out), 2.0);
}

TEST_F(TableTest, GetOutputParamUnchangedOnMiss) {
    Value out = Value{99.0};
    tableGet(&table, str("missing"), out);
    EXPECT_DOUBLE_EQ(as<double>(out), 99.0);
}

// ============================================================
// 5. tableDelete — observable removal
// ============================================================

TEST_F(TableTest, DeleteFromEmptyReturnsFalse) {
    EXPECT_FALSE(tableDelete(&table, str("missing")));
}

TEST_F(TableTest, DeleteMissingKeyReturnsFalse) {
    tableSet(&table, str("foo"), Value{1.0});
    EXPECT_FALSE(tableDelete(&table, str("bar")));
}

TEST_F(TableTest, DeleteExistingKeyReturnsTrue) {
    ObjString* key = str("foo");
    tableSet(&table, key, Value{1.0});
    EXPECT_TRUE(tableDelete(&table, key));
}

TEST_F(TableTest, DeletedKeyIsNoLongerFound) {
    ObjString* key = str("foo");
    tableSet(&table, key, Value{1.0});
    tableDelete(&table, key);
    Value out;
    EXPECT_FALSE(tableGet(&table, key, out));
}

TEST_F(TableTest, DeleteDoesNotOrphanCollidingKey) {
    // Insert two keys that land in the same bucket, delete one,
    // the other must still be findable.
    auto [a, b] = collidingPair();
    tableSet(&table, a, Value{1.0});
    tableSet(&table, b, Value{2.0});
    tableDelete(&table, a);
    Value out;
    EXPECT_TRUE(tableGet(&table, b, out));
    EXPECT_DOUBLE_EQ(as<double>(out), 2.0);
}

TEST_F(TableTest, DeleteThenReinsertSameKey) {
    ObjString* key = str("foo");
    tableSet(&table, key, Value{1.0});
    tableDelete(&table, key);
    EXPECT_TRUE(tableSet(&table, key, Value{3.0}));
    Value out;
    EXPECT_TRUE(tableGet(&table, key, out));
    EXPECT_DOUBLE_EQ(as<double>(out), 3.0);
}

TEST_F(TableTest, DeleteSeveralFromCollisionChainRemainingStillFound) {
    // Three-way collision: delete first two, third must still be reachable.
    auto keys = collidingN(3);
    for (size_t i = 0; i < keys.size(); ++i) {
        tableSet(&table, keys[i], Value{static_cast<double>(i)});
    }
    tableDelete(&table, keys[0]);
    tableDelete(&table, keys[1]);
    Value out;
    EXPECT_TRUE(tableGet(&table, keys[2], out));
    EXPECT_DOUBLE_EQ(as<double>(out), 2.0);
}

// ============================================================
// 6. Probing / collision correctness
// ============================================================

TEST_F(TableTest, TwoCollidingKeysBothFindable) {
    auto [a, b] = collidingPair();
    tableSet(&table, a, Value{10.0});
    tableSet(&table, b, Value{20.0});
    Value va, vb;
    EXPECT_TRUE(tableGet(&table, a, va));
    EXPECT_TRUE(tableGet(&table, b, vb));
    EXPECT_DOUBLE_EQ(as<double>(va), 10.0);
    EXPECT_DOUBLE_EQ(as<double>(vb), 20.0);
}

TEST_F(TableTest, ThreeCollidingKeysAllFindable) {
    auto keys = collidingN(3);
    for (size_t i = 0; i < keys.size(); ++i) {
        tableSet(&table, keys[i], Value{static_cast<double>(i)});
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        Value out;
        EXPECT_TRUE(tableGet(&table, keys[i], out)) << "key[" << i << "] missing";
        EXPECT_DOUBLE_EQ(as<double>(out), static_cast<double>(i));
    }
}

TEST_F(TableTest, MixedCollidingAndNonCollidingKeys) {
    auto [a, b] = collidingPair();
    ObjString* c = str("unique_xyz_key");
    tableSet(&table, a, Value{10.0});
    tableSet(&table, b, Value{20.0});
    tableSet(&table, c, Value{30.0});
    Value va, vb, vc;
    EXPECT_TRUE(tableGet(&table, a, va));
    EXPECT_DOUBLE_EQ(as<double>(va), 10.0);
    EXPECT_TRUE(tableGet(&table, b, vb));
    EXPECT_DOUBLE_EQ(as<double>(vb), 20.0);
    EXPECT_TRUE(tableGet(&table, c, vc));
    EXPECT_DOUBLE_EQ(as<double>(vc), 30.0);
}

TEST_F(TableTest, StressManyInsertsAllRetrievable) {
    const int N = 200;
    std::vector<ObjString*> keys;
    for (int i = 0; i < N; ++i) {
        keys.push_back(str("stress" + std::to_string(i)));
        tableSet(&table, keys.back(), Value{static_cast<double>(i)});
    }
    for (int i = 0; i < N; ++i) {
        Value out;
        EXPECT_TRUE(tableGet(&table, keys[i], out)) << "stress" << i << " missing";
        EXPECT_DOUBLE_EQ(as<double>(out), static_cast<double>(i));
    }
}

// ============================================================
// 7. tableAddAll
// ============================================================

TEST_F(TableTest, AddAllEmptySourceLeavesDestUnchanged) {
    Table src;
    initTable(&src);
    tableAddAll(&src, &table);
    Value out;
    EXPECT_FALSE(tableGet(&table, str("any"), out));
    freeTable(&src);
}

TEST_F(TableTest, AddAllCopiesEntriesToEmptyDest) {
    std::vector<std::unique_ptr<Obj>> srcObjs;
    Table src;
    initTable(&src);
    auto srcStr = [&](const std::string& s) {
        ObjString* obj = makeString(srcObjs, s);
        obj->hash = fakeHash(s.c_str(), static_cast<int>(s.size()));
        return obj;
    };
    ObjString* k1 = srcStr("alpha");
    ObjString* k2 = srcStr("beta");
    tableSet(&src, k1, Value{1.0});
    tableSet(&src, k2, Value{2.0});

    tableAddAll(&src, &table);

    Value v1, v2;
    EXPECT_TRUE(tableGet(&table, k1, v1));
    EXPECT_DOUBLE_EQ(as<double>(v1), 1.0);
    EXPECT_TRUE(tableGet(&table, k2, v2));
    EXPECT_DOUBLE_EQ(as<double>(v2), 2.0);
    freeTable(&src);
}

TEST_F(TableTest, AddAllNoOverlapUnionInDest) {
    std::vector<std::unique_ptr<Obj>> srcObjs;
    Table src;
    initTable(&src);

    ObjString* destKey = str("dest_only");
    tableSet(&table, destKey, Value{9.0});

    ObjString* srcKey = makeString(srcObjs, "src_only");
    srcKey->hash = fakeHash("src_only", 8);
    tableSet(&src, srcKey, Value{7.0});

    tableAddAll(&src, &table);

    Value vd, vs;
    EXPECT_TRUE(tableGet(&table, destKey, vd));
    EXPECT_DOUBLE_EQ(as<double>(vd), 9.0);
    EXPECT_TRUE(tableGet(&table, srcKey, vs));
    EXPECT_DOUBLE_EQ(as<double>(vs), 7.0);
    freeTable(&src);
}

TEST_F(TableTest, AddAllOverlappingKeysTakesSourceValue) {
    Table src;
    initTable(&src);
    // Use the same key pointer in both tables
    ObjString* sharedKey = str("shared");
    tableSet(&table, sharedKey, Value{1.0});
    tableSet(&src, sharedKey, Value{99.0});

    tableAddAll(&src, &table);

    Value out;
    tableGet(&table, sharedKey, out);
    EXPECT_DOUBLE_EQ(as<double>(out), 99.0);
    freeTable(&src);
}

TEST_F(TableTest, AddAllDoesNotMutateSource) {
    std::vector<std::unique_ptr<Obj>> srcObjs;
    Table src;
    initTable(&src);
    ObjString* k = makeString(srcObjs, "orig");
    k->hash = fakeHash("orig", 4);
    tableSet(&src, k, Value{5.0});

    tableAddAll(&src, &table);

    Value out;
    EXPECT_TRUE(tableGet(&src, k, out));
    EXPECT_DOUBLE_EQ(as<double>(out), 5.0);
    freeTable(&src);
}

// ============================================================
// 8. tableFindString — raw character lookup
// ============================================================

TEST_F(TableTest, FindStringInEmptyTableReturnsNull) {
    uint32_t h = fakeHash("hello", 5);
    EXPECT_EQ(tableFindString(&table, "hello", 5, h), nullptr);
}

TEST_F(TableTest, FindStringAbsentReturnsNull) {
    ObjString* k = str("present");
    tableSet(&table, k, Value{std::monostate{}});
    uint32_t h = fakeHash("absent", 6);
    EXPECT_EQ(tableFindString(&table, "absent", 6, h), nullptr);
}

TEST_F(TableTest, FindStringPresentReturnsPointer) {
    ObjString* k = str("hello");
    tableSet(&table, k, Value{std::monostate{}});
    uint32_t h = fakeHash("hello", 5);
    EXPECT_EQ(tableFindString(&table, "hello", 5, h), k);
}

TEST_F(TableTest, FindStringHashCollisionDifferentContentReturnsNull) {
    // Insert only `a`; `b` collides but is absent — must not return `a`.
    auto [a, b] = collidingPair();
    tableSet(&table, a, Value{std::monostate{}});
    EXPECT_EQ(tableFindString(&table, b->chars.c_str(),
                              static_cast<int>(b->chars.size()), b->hash),
              nullptr);
}

TEST_F(TableTest, FindStringByRawCharsWithoutObjStringWrapper) {
    ObjString* k = str("lookup_me");
    tableSet(&table, k, Value{std::monostate{}});
    const char* raw = "lookup_me";
    int len = static_cast<int>(k->chars.size());
    uint32_t h = fakeHash(raw, len);
    EXPECT_EQ(tableFindString(&table, raw, len, h), k);
}

// ============================================================
// 9. String interning (using Table as intern table directly)
// ============================================================

class StringInternTest : public ::testing::Test {
  protected:
    std::vector<std::unique_ptr<Obj>> objects;
    Table internTable;

    void SetUp() override { initTable(&internTable); }
    void TearDown() override { freeTable(&internTable); }

    ObjString* mkstr(const std::string& s) {
        ObjString* obj = makeString(objects, s);
        obj->hash = fakeHash(s.c_str(), static_cast<int>(s.size()));
        return obj;
    }

    ObjString* internString(const std::string& s) {
        uint32_t h = fakeHash(s.c_str(), static_cast<int>(s.size()));
        ObjString* found = tableFindString(&internTable, s.c_str(),
                                           static_cast<int>(s.size()), h);
        if (found) return found;
        ObjString* obj = mkstr(s);
        tableSet(&internTable, obj, Value{std::monostate{}});
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
