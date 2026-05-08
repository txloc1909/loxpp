#include "test_harness.h"
#include "value.h"

#include <gtest/gtest.h>

static void expect_num(Value v, double n) {
    ASSERT_TRUE(is<Number>(v));
    EXPECT_DOUBLE_EQ(as<Number>(v), n);
}

static void expect_str(Value v, const char* s) {
    ASSERT_TRUE(isString(v));
    EXPECT_EQ(asObjString(as<Obj*>(v))->chars.c_str(), std::string(s));
}

class ListPatternTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// Empty pattern
// ---------------------------------------------------------------------------

TEST_F(ListPatternTest, EmptyListMatches) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [] {
            case []  => 1
            case _   => 0
        };
        var y = match [1] {
            case []  => 1
            case _   => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 1);
    expect_num(*h.getGlobal("y"), 0);
}

TEST_F(ListPatternTest, EmptyStringMatches) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match "" {
            case [] => 1
            case _  => 0
        };
        var y = match "a" {
            case [] => 1
            case _  => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 1);
    expect_num(*h.getGlobal("y"), 0);
}

// ---------------------------------------------------------------------------
// Fixed-length patterns
// ---------------------------------------------------------------------------

TEST_F(ListPatternTest, FixedLengthBindings) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [10, 20] {
            case [a, b] => a + b
            case _      => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 30);
}

TEST_F(ListPatternTest, ThreeElementBindings) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [1, 2, 3] {
            case [a, b, c] => a * 100 + b * 10 + c
            case _         => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 123);
}

TEST_F(ListPatternTest, WildcardSkip) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [5, 99, 7] {
            case [a, _, b] => a + b
            case _         => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 12);
}

TEST_F(ListPatternTest, AllWildcards) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [1, 2] {
            case [_, _] => 42
            case _      => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 42);
}

TEST_F(ListPatternTest, LengthMismatchMisses) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [1, 2, 3] {
            case [a, b] => 1
            case _      => 2
        };
        var y = match [1] {
            case [a, b] => 1
            case _      => 2
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 2);
    expect_num(*h.getGlobal("y"), 2);
}

TEST_F(ListPatternTest, TypeMismatchMisses) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match 42 {
            case [a] => 1
            case _   => 2
        };
        var y = match nil {
            case [a, b] => 1
            case _      => 2
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 2);
    expect_num(*h.getGlobal("y"), 2);
}

// ---------------------------------------------------------------------------
// Rest patterns
// ---------------------------------------------------------------------------

TEST_F(ListPatternTest, RestPatternBasic) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var lst = [1, 2, 3, 4];
        var h = match lst {
            case [head, ...tail] => head
            case _               => 0
        };
        var tlen = match lst {
            case [head, ...tail] => len(tail)
            case _               => -1
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("h"), 1);
    expect_num(*h.getGlobal("tlen"), 3);
}

TEST_F(ListPatternTest, RestPatternTwoFixed) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [10, 20, 30, 40] {
            case [a, b, ...rest] => a + b + len(rest)
            case _               => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 32); // 10 + 20 + 2
}

TEST_F(ListPatternTest, RestPatternOnEmptyTail) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [5] {
            case [h, ...t] => len(t)
            case _         => -1
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 0);
}

TEST_F(ListPatternTest, RestOnlyPattern) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [1, 2, 3] {
            case [...all] => len(all)
            case _        => 0
        };
        var y = match [] {
            case [...all] => len(all)
            case _        => -1
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 3);
    expect_num(*h.getGlobal("y"), 0);
}

TEST_F(ListPatternTest, RestPatternMissesWhenTooShort) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [] {
            case [h, ...t] => 1
            case _         => 2
        };
        var y = match [1] {
            case [a, b, ...t] => 1
            case _            => 2
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 2);
    expect_num(*h.getGlobal("y"), 2);
}

TEST_F(ListPatternTest, RestPatternDiscardRest) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [7, 8, 9] {
            case [first, ..._] => first
            case _             => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 7);
}

// ---------------------------------------------------------------------------
// String sequences
// ---------------------------------------------------------------------------

TEST_F(ListPatternTest, StringFixedLength) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match "ab" {
            case [a, b] => 1
            case _      => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 1);
}

TEST_F(ListPatternTest, StringRestPattern) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var tlen = match "hello" {
            case [h, ...t] => len(t)
            case _         => -1
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("tlen"), 4);
}

// ---------------------------------------------------------------------------
// Guard clauses
// ---------------------------------------------------------------------------

TEST_F(ListPatternTest, GuardOnListArm) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match [5, 3] {
            case [a, b] if a > b => a - b
            case [a, b]          => b - a
            case _               => 0
        };
        var y = match [3, 5] {
            case [a, b] if a > b => a - b
            case [a, b]          => b - a
            case _               => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 2);
    expect_num(*h.getGlobal("y"), 2);
}

// ---------------------------------------------------------------------------
// Multiple arms with fall-through
// ---------------------------------------------------------------------------

TEST_F(ListPatternTest, MultipleArmsFallThrough) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun classify(lst) {
            return match lst {
                case []         => "empty"
                case [x]        => "singleton"
                case [x, y]     => "pair"
                case [h, ...t]  => "many"
            };
        }
        var a = classify([]);
        var b = classify([1]);
        var c = classify([1, 2]);
        var d = classify([1, 2, 3]);
    )"),
              InterpretResult::OK);
    expect_str(*h.getGlobal("a"), "empty");
    expect_str(*h.getGlobal("b"), "singleton");
    expect_str(*h.getGlobal("c"), "pair");
    expect_str(*h.getGlobal("d"), "many");
}

// ---------------------------------------------------------------------------
// MatchError when no arm matches
// ---------------------------------------------------------------------------

TEST_F(ListPatternTest, MatchErrorRaised) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        match [1, 2, 3] {
            case [x] => x
        };
    )"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// Local scope / nested in function
// ---------------------------------------------------------------------------

TEST_F(ListPatternTest, InsideFunction) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun sum2(lst) {
            return match lst {
                case [a, b] => a + b
                case _      => 0
            };
        }
        var r1 = sum2([3, 4]);
        var r2 = sum2([1]);
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r1"), 7);
    expect_num(*h.getGlobal("r2"), 0);
}

TEST_F(ListPatternTest, InsideNestedScopes) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var result = 0;
        {
            var lst = [10, 20];
            result = match lst {
                case [a, b] => a * b
                case _      => -1
            };
        }
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("result"), 200);
}
