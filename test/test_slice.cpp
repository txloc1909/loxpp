// test_slice.cpp — Tests for seq[start:end] slice expressions on List and String.

#include "test_harness.h"
#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// List — basic slices
// ---------------------------------------------------------------------------

TEST(Slice, ListMiddle) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30, 40, 50]; var r = xs[1:3];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[20, 30]");
}

TEST(Slice, ListFromStart) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30, 40, 50]; var r = xs[0:2];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[10, 20]");
}

TEST(Slice, ListToEnd) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30, 40, 50]; var r = xs[2:5];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[30, 40, 50]");
}

TEST(Slice, ListFullCopy) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30]; var r = xs[0:3];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[10, 20, 30]");
}

TEST(Slice, ListSingleElement) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30]; var r = xs[1:2];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[20]");
}

// ---------------------------------------------------------------------------
// List — empty results
// ---------------------------------------------------------------------------

TEST(Slice, ListEmptyStartEqualsEnd) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30]; var r = xs[2:2];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[]");
}

TEST(Slice, ListEmptyStartGtEnd) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30]; var r = xs[3:1];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[]");
}

TEST(Slice, ListEmptyOnEmptyList) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = []; var r = xs[0:0];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[]");
}

// ---------------------------------------------------------------------------
// List — out-of-range clamping
// ---------------------------------------------------------------------------

TEST(Slice, ListEndClampedToLen) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2]; var r = xs[0:100];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[1, 2]");
}

TEST(Slice, ListStartClampedToLen) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2]; var r = xs[10:20];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[]");
}

TEST(Slice, ListBothBoundsLarger) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [1, 2, 3]; var r = xs[2:100];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[3]");
}

// ---------------------------------------------------------------------------
// List — produces a new independent list
// ---------------------------------------------------------------------------

TEST(Slice, ListSliceIsNewObject) {
    // The slice result must not be identity-equal to the original.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [1, 2, 3];
        var ys = xs[0:3];
        var same = xs == ys;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("same"), "false");
}

TEST(Slice, ListSliceMutationIndependent) {
    // Mutating the slice must not affect the original.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [1, 2, 3];
        var ys = xs[0:3];
        ys[0] = 99;
        var orig = xs[0];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("orig"), "1");
}

// ---------------------------------------------------------------------------
// List — slice of heterogeneous list
// ---------------------------------------------------------------------------

TEST(Slice, ListHeterogeneousSlice) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var xs = [1, "hello", true, nil]; var r = xs[1:3];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[hello, true]");
}

// ---------------------------------------------------------------------------
// List — expression indices
// ---------------------------------------------------------------------------

TEST(Slice, ListExpressionIndices) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [10, 20, 30, 40, 50];
        var i = 1;
        var j = 4;
        var r = xs[i:j];
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[20, 30, 40]");
}

TEST(Slice, ListIntegerValuedFloat) {
    // 1.0 and 3.0 are the same as 1 and 3 — must be accepted
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30, 40]; var r = xs[1.0:3.0];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[20, 30]");
}

// ---------------------------------------------------------------------------
// List — chained operations on slice result
// ---------------------------------------------------------------------------

TEST(Slice, ListSliceChainedIndex) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30, 40]; var r = xs[1:4][1];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "30");
}

TEST(Slice, ListSliceChainedSlice) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30, 40, 50]; var r = xs[1:5][0:2];"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "[20, 30]");
}

// ---------------------------------------------------------------------------
// String — basic slices
// ---------------------------------------------------------------------------

TEST(Slice, StringMiddle) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hello"; var r = s[1:4];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "ell");
}

TEST(Slice, StringFromStart) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hello"; var r = s[0:3];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "hel");
}

TEST(Slice, StringToEnd) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hello"; var r = s[2:5];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "llo");
}

TEST(Slice, StringFullCopy) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hello"; var r = s[0:5];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "hello");
}

TEST(Slice, StringSingleChar) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hello"; var r = s[1:2];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "e");
}

// ---------------------------------------------------------------------------
// String — empty results
// ---------------------------------------------------------------------------

TEST(Slice, StringEmptyStartEqualsEnd) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hi"; var r = s[1:1];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "");
}

TEST(Slice, StringEmptyStartGtEnd) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hi"; var r = s[2:0];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "");
}

TEST(Slice, StringEmptyOnEmptyString) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = ""; var r = s[0:0];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "");
}

// ---------------------------------------------------------------------------
// String — out-of-range clamping
// ---------------------------------------------------------------------------

TEST(Slice, StringEndClampedToLen) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hi"; var r = s[0:100];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "hi");
}

TEST(Slice, StringStartClampedToLen) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hi"; var r = s[10:20];)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "");
}

// ---------------------------------------------------------------------------
// String — result is a new value (equality by content)
// ---------------------------------------------------------------------------

TEST(Slice, StringSliceContentEqual) {
    // Slice of "hello"[1:4] == "ell" — equal by content.
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var s = "hello"; var r = s[1:4] == "ell";)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "true");
}

// ---------------------------------------------------------------------------
// Runtime errors — wrong type for seq
// ---------------------------------------------------------------------------

TEST(Slice, MapSliceIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(var m = {"a": 1}; var r = m[0:1];)"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(Slice, NumberSliceIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var r = 42[0:1];"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// Runtime errors — wrong index type
// ---------------------------------------------------------------------------

TEST(Slice, ListStringIndexIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(var xs = [1,2,3]; var r = xs["a":1];)"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(Slice, ListNonIntegerIndexIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var xs = [1,2,3]; var r = xs[1.5:3];"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(Slice, ListNegativeStartIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var xs = [1,2,3]; var r = xs[-1:2];"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(Slice, ListNegativeEndIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("var xs = [1,2,3]; var r = xs[0:-1];"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(Slice, StringNegativeIndexIsRuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(var s = "hello"; var r = s[-1:3];)"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// Slice result can be iterated with for-in
// ---------------------------------------------------------------------------

TEST(Slice, ListSliceForIn) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var xs = [10, 20, 30, 40, 50];
        var sum = 0;
        for (var x in xs[1:4]) {
            sum = sum + x;
        }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("sum"), "90");
}

TEST(Slice, StringSliceForIn) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var s = "hello";
        var out = "";
        for (var c in s[1:4]) {
            out = out + c;
        }
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("out"), "ell");
}

// ---------------------------------------------------------------------------
// Slice result supports len()
// ---------------------------------------------------------------------------

TEST(Slice, ListSliceLen) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var xs = [10, 20, 30, 40, 50]; var r = len(xs[1:4]);"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3");
}

TEST(Slice, StringSliceLen) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(var r = len("hello"[1:4]);)"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("r"), "3");
}
