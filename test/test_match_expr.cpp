#include "test_harness.h"
#include "function.h"
#include "value.h"
#include <gtest/gtest.h>

static void expect_num(Value v, double n) {
    ASSERT_TRUE(is<Number>(v));
    EXPECT_DOUBLE_EQ(as<Number>(v), n);
}

static void expect_str(Value v, const char* s) {
    ASSERT_TRUE(is<Obj*>(v));
    ASSERT_EQ(as<Obj*>(v)->type, ObjType::STRING);
    EXPECT_EQ(asObjString(as<Obj*>(v))->chars.c_str(), std::string(s));
}

class MatchExpressionTest : public ::testing::Test {};

TEST_F(MatchExpressionTest, LiteralArm) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match 1 {
            case 1 => 42
            case _ => 0
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42);
}

TEST_F(MatchExpressionTest, EnumConstructor) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Result { Ok(value) Err(msg) }
        var x = match Ok(5) {
            case Ok(v) => v
            case Err(m) => 0
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 5);
}

TEST_F(MatchExpressionTest, GuardPass) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Result { Ok(value) Err(msg) }
        var x = match Ok(5) {
            case Ok(v) if v > 3 => 1
            case Ok(v) => 2
            case Err(m) => 3
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1);
}

TEST_F(MatchExpressionTest, BracedBody) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match 1 {
            case 1 => { var t = 10; t * 2 }
            case _ => 0
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 20);
}

TEST_F(MatchExpressionTest, StackNeutral) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = 42;
        match x {
            case 42 => 100
            case _ => 200
        };
        var y = 99;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.stackDepth(), 0);
}

TEST_F(MatchExpressionTest, MatchInFunctionReturn) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        fun f(v) {
            return match v {
                case 1 => "one"
                case 2 => "two"
                case _ => "other"
            };
        }
        var x = f(1);
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    expect_str(*v, "one");
}

TEST_F(MatchExpressionTest, NoMatchRaisesError) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var x = match 5 {
            case 1 => 10
        };
    )"),
              InterpretResult::RUNTIME_ERROR);
}

// Exercises issue #68: var label = match expr { ... } inside a loop at local
// scope. The pre-existing slot-corruption bug causes wrong values on the second
// and subsequent iterations.
TEST_F(MatchExpressionTest, LoopLocalSlotCorruption) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Result { Ok(v) Err(msg) }
        var out = 0;
        var i = 0;
        while (i < 2) {
            i = i + 1;
            var r = match Ok(i) {
                case Ok(v) => v
                case Err(m) => -1
            };
            out = out + r;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("out");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 3); // 1 + 2
}
