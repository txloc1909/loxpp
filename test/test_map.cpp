// test_map.cpp — Tests for the Map type: literals, indexing, methods, GC.

#include "test_harness.h"
#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// Map literals
// ---------------------------------------------------------------------------

TEST(Map, EmptyLiteral) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = {}; print m;"), InterpretResult::OK);
}

TEST(Map, EmptyStringify) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = {};"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("m"), "{}");
}

TEST(Map, StringKey) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1}; var r = m["a"];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "1");
}

TEST(Map, NumberKey) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = {1: \"one\"}; var r = m[1];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "one");
}

TEST(Map, BoolKey) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = {true: \"yes\", false: \"no\"}; var r = m[true];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "yes");
}

TEST(Map, NilKey) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = {nil: \"nothing\"}; var r = m[nil];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "nothing");
}

TEST(Map, MixedKeys) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var m = {"x": 1, 2: "two", true: 3, nil: 4};
        var a = m["x"];
        var b = m[2];
        var c = m[true];
        var d = m[nil];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("a"), "1");
    EXPECT_EQ(h.getGlobalStr("b"), "two");
    EXPECT_EQ(h.getGlobalStr("c"), "3");
    EXPECT_EQ(h.getGlobalStr("d"), "4");
}

// ---------------------------------------------------------------------------
// Index get
// ---------------------------------------------------------------------------

TEST(Map, GetMissingKeyReturnsNil) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = {}; var r = m[\"missing\"];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "nil");
}

TEST(Map, GetAfterSet) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {}; m["k"] = 42; var r = m["k"];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "42");
}

// ---------------------------------------------------------------------------
// Index set
// ---------------------------------------------------------------------------

TEST(Map, SetIsExpression) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {}; var r = m["k"] = 99;)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "99");
}

TEST(Map, UpdateExistingKey) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"k": 1}; m["k"] = 2; var r = m["k"];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "2");
}

TEST(Map, SetManyKeys) {
    // Force a rehash by inserting more than the initial capacity.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var m = {};
        var i = 0;
        while (i < 20) {
            m[i] = i * 2;
            i = i + 1;
        }
        var r0 = m[0];
        var r9 = m[9];
        var r19 = m[19];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r0"), "0");
    EXPECT_EQ(h.getGlobalStr("r9"), "18");
    EXPECT_EQ(h.getGlobalStr("r19"), "38");
}

// ---------------------------------------------------------------------------
// len()
// ---------------------------------------------------------------------------

TEST(Map, LenEmpty) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var r = len({});"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "0");
}

TEST(Map, LenAfterInserts) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1, "b": 2, "c": 3}; var r = len(m);)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3");
}

TEST(Map, LenAfterDuplicate) {
    // Inserting the same key twice must not increase len.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"k": 1}; m["k"] = 2; var r = len(m);)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "1");
}

// ---------------------------------------------------------------------------
// has() method
// ---------------------------------------------------------------------------

TEST(Map, HasExistingKey) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1}; var r = m.has("a");)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

TEST(Map, HasMissingKey) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1}; var r = m.has("b");)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "false");
}

TEST(Map, HasAfterDel) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1}; m.del("a"); var r = m.has("a");)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "false");
}

// ---------------------------------------------------------------------------
// del() method
// ---------------------------------------------------------------------------

TEST(Map, DelReducesLen) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1, "b": 2}; m.del("a"); var r = len(m);)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "1");
}

TEST(Map, DelMissingKeyNoError) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {}; m.del("x");)"), InterpretResult::OK);
}

TEST(Map, DelThenReinsert) {
    // Tombstone reuse: delete a key then reinsert it.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var m = {"k": 1};
        m.del("k");
        m["k"] = 2;
        var r = m["k"];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "2");
}

// ---------------------------------------------------------------------------
// in operator
// ---------------------------------------------------------------------------

TEST(Map, InExistingKey) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1}; var r = "a" in m;)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

TEST(Map, InMissingKey) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1}; var r = "b" in m;)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "false");
}

// ---------------------------------------------------------------------------
// keys() / values() / entries()
// ---------------------------------------------------------------------------

TEST(Map, KeysLength) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1, "b": 2}; var r = len(m.keys());)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "2");
}

TEST(Map, ValuesLength) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1, "b": 2}; var r = len(m.values());)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "2");
}

TEST(Map, EntriesLength) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var m = {"a": 1, "b": 2}; var r = len(m.entries());)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "2");
}

TEST(Map, EntriesPairStructure) {
    VMTestHarness h;
    // Single entry — the pair must be a 2-element list [key, value].
    ASSERT_EQ(h.run(R"(
        var m = {"x": 99};
        var es = m.entries();
        var pair = es[0];
        var k = pair[0];
        var v = pair[1];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("k"), "x");
    EXPECT_EQ(h.getGlobalStr("v"), "99");
}

// ---------------------------------------------------------------------------
// Error cases
// ---------------------------------------------------------------------------

TEST(Map, NaNKeyRejectedOnLiteral) {
    VMTestHarness h;
    // 0/0 == NaN; BUILD_MAP must reject it.
    ASSERT_EQ(h.run("var m = {0/0: 1};"), InterpretResult::RUNTIME_ERROR);
}

TEST(Map, NaNKeyRejectedOnSet) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = {}; m[0/0] = 1;"), InterpretResult::RUNTIME_ERROR);
}

TEST(Map, ObjectKeyRejected) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = {}; m[[1,2,3]] = 1;"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// for-in iteration
// ---------------------------------------------------------------------------

TEST(Map, ForInEmpty) {
    VMTestHarness h;
    // No iterations for an empty map.
    ASSERT_EQ(h.run(R"(
        var count = 0;
        for (var k in {}) {
            count = count + 1;
        }
        var r = count;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "0");
}

TEST(Map, ForInCollectsAllKeys) {
    VMTestHarness h;
    // Sum all numeric keys to verify every key is visited exactly once.
    ASSERT_EQ(h.run(R"(
        var m = {1: "a", 2: "b", 3: "c"};
        var sum = 0;
        for (var k in m) {
            sum = sum + k;
        }
        var r = sum;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "6");
}
