// test_stringify_depth_guard.cpp — Tests for stringifyObj recursion depth
// guard.

#include "test_harness.h"
#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// Stringify depth guard
// ---------------------------------------------------------------------------

TEST(StringifyDepthGuard, NestedListTripsGuard) {
    // Build a deeply nested list that exceeds kMaxStringifyDepth (200).
    // The guard should trigger and produce a controlled LoxError, not a crash.
    // This reproduces the crash from issue #152 with N=27000.
    VMTestHarness h;
    std::string source = R"(
        var x = 0;
        var N = 250;  // Deep enough to trip the guard at 200
        var i = 0;
        while (i < N) {
            x = [x];
            i = i + 1;
        }
        print x;
    )";
    // The print should trigger the depth guard and return exit code 70
    // (LOXERR70)
    InterpretResult result = h.run(source);
    // We expect this to fail because of the error, not to segfault
    EXPECT_NE(result, InterpretResult::OK);
}

TEST(StringifyDepthGuard, NestedMapTripsGuard) {
    // Build a deeply nested map structure and verify it also triggers the
    // guard.
    VMTestHarness h;
    std::string source = R"(
        var x = {};
        var N = 250;  // Deep enough to trip the guard at 200
        var i = 0;
        while (i < N) {
            x = {"value": x};
            i = i + 1;
        }
        print x;
    )";
    InterpretResult result = h.run(source);
    EXPECT_NE(result, InterpretResult::OK);
}

TEST(StringifyDepthGuard, NestedEnumTripsGuard) {
    // Build a deeply nested enum value chain and verify the guard applies.
    VMTestHarness h;
    std::string source = R"(
        enum Node { Leaf, Branch with value }
        var x = Node.Leaf;
        var N = 250;  // Deep enough to trip the guard at 200
        var i = 0;
        while (i < N) {
            x = Node.Branch(x);
            i = i + 1;
        }
        print x;
    )";
    InterpretResult result = h.run(source);
    EXPECT_NE(result, InterpretResult::OK);
}

TEST(StringifyDepthGuard, DepthCounterResetsAfterError) {
    // Verify the most critical invariant from the node spec: the depth counter
    // is correctly reset even when an error is thrown out of a deeply nested
    // call. Run two programs in the same process: one that trips the guard, one
    // shallow. The second must succeed, proving the counter was reset.
    VMTestHarness h;

    // First program: deeply nested list that trips the guard
    std::string deepSource = R"(
        var x = 0;
        var N = 250;
        var i = 0;
        while (i < N) {
            x = [x];
            i = i + 1;
        }
        print x;
    )";
    InterpretResult deepResult = h.run(deepSource);
    EXPECT_NE(deepResult, InterpretResult::OK);

    // Second program: shallow structure that must succeed
    // This runs in the same harness (same process), so if the counter was not
    // reset, this would also fail.
    std::string shallowSource = R"(
        var shallow = [[[[5]]]];
        print shallow;
    )";
    InterpretResult shallowResult = h.run(shallowSource);
    EXPECT_EQ(shallowResult, InterpretResult::OK);
}

TEST(StringifyDepthGuard, ShallowStructureStillPrints) {
    // Verify that normal, shallow nested structures are not affected by the
    // guard. The guard at 200 should not fire on ordinary programs.
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = [[[[5]]]]; print x;"), InterpretResult::OK);
    // Verify the output is the canonical form (unchanged by the guard)
    EXPECT_EQ(h.getGlobalStr("x"), "[[[[5]]]]");
}

TEST(StringifyDepthGuard, MapShallowStructureStillPrints) {
    // Verify shallow maps are not affected.
    VMTestHarness h;
    ASSERT_EQ(h.run("var x = {a: {b: {c: 1}}}; print x;"), InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("x"), "{a: {b: {c: 1}}}");
}

TEST(StringifyDepthGuard, StringifyViaStrFunction) {
    // Verify the guard also applies when stringifying via the str() function
    // (the callNative path mentioned in the review).
    VMTestHarness h;
    std::string source = R"(
        var x = 0;
        var N = 250;
        var i = 0;
        while (i < N) {
            x = [x];
            i = i + 1;
        }
        var s = str(x);
        print s;
    )";
    InterpretResult result = h.run(source);
    EXPECT_NE(result, InterpretResult::OK);
}
