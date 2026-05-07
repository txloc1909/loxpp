#include "test_harness.h"
#include "value.h"

#include <gtest/gtest.h>

static void expect_num(Value v, double n) {
    ASSERT_TRUE(is<Number>(v));
    EXPECT_DOUBLE_EQ(as<Number>(v), n);
}

// ---------------------------------------------------------------------------
// OrPatternTest — or-separated alternatives in match arms
// ---------------------------------------------------------------------------

class OrPatternTest : public ::testing::Test {};

// Zero-binding: two zero-field constructors share one arm.
TEST_F(OrPatternTest, ZeroBindingTwoAlts) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Msg { Quit Pause Resume }
        var a = match Quit() {
            case Quit or Pause => 1
            case Resume        => 2
        };
        var b = match Pause() {
            case Quit or Pause => 1
            case Resume        => 2
        };
        var c = match Resume() {
            case Quit or Pause => 1
            case Resume        => 2
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 1);
    expect_num(*h.getGlobal("b"), 1);
    expect_num(*h.getGlobal("c"), 2);
}

// Binding or-pattern: both alternatives bind the same name.
TEST_F(OrPatternTest, BindingTwoAlts) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Ev { Move(x) Teleport(x) Stop }
        var a = match Move(10) {
            case Move(x) or Teleport(x) => x
            case Stop                   => 0
        };
        var b = match Teleport(42) {
            case Move(x) or Teleport(x) => x
            case Stop                   => 0
        };
        var c = match Stop() {
            case Move(x) or Teleport(x) => x
            case Stop                   => 0
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 10);
    expect_num(*h.getGlobal("b"), 42);
    expect_num(*h.getGlobal("c"), 0);
}

// Three-alternative or-pattern.
TEST_F(OrPatternTest, ThreeAlts) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Color { Red Green Blue Alpha }
        var a = match Red() {
            case Red or Green or Blue => 1
            case Alpha                => 2
        };
        var b = match Green() {
            case Red or Green or Blue => 1
            case Alpha                => 2
        };
        var c = match Blue() {
            case Red or Green or Blue => 1
            case Alpha                => 2
        };
        var d = match Alpha() {
            case Red or Green or Blue => 1
            case Alpha                => 2
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 1);
    expect_num(*h.getGlobal("b"), 1);
    expect_num(*h.getGlobal("c"), 1);
    expect_num(*h.getGlobal("d"), 2);
}

// Or-pattern with guard: guard sees the binding from the matched alternative.
TEST_F(OrPatternTest, WithGuard) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Ev { Move(x) Teleport(x) Stop }
        var a = match Move(5) {
            case Move(x) or Teleport(x) if x > 3 => 1
            case Move(x) or Teleport(x)           => 2
            case Stop                              => 3
        };
        var b = match Teleport(1) {
            case Move(x) or Teleport(x) if x > 3 => 1
            case Move(x) or Teleport(x)           => 2
            case Stop                              => 3
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 1);
    expect_num(*h.getGlobal("b"), 2);
}

// Or-pattern counts all constructors for exhaustiveness.
TEST_F(OrPatternTest, ExhaustivenessViaOrPattern) {
    VMTestHarness h;
    // Two arms, one with or-pattern, covering all three constructors.
    ASSERT_EQ(h.run(R"(
        enum Shape { Circle Rect Triangle }
        var x = match Circle() {
            case Circle or Rect => 1
            case Triangle       => 2
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 1);
}

// Or-pattern with named-field bindings.
TEST_F(OrPatternTest, NamedFieldBinding) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Shape { Circle(r) Square(r) Rect(w) }
        var a = match Circle(7) {
            case Circle{r} or Square{r} => r
            case Rect{w}                => w
        };
        var b = match Square(3) {
            case Circle{r} or Square{r} => r
            case Rect{w}                => w
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 7);
    expect_num(*h.getGlobal("b"), 3);
}

// Class pattern or-alternatives.
TEST_F(OrPatternTest, ClassPatternOrAlts) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Cat  { init(age) { this.age = age; } }
        class Dog  { init(age) { this.age = age; } }
        class Fish { init(age) { this.age = age; } }
        var pet = Cat(3);
        var x = match pet {
            case Cat{age} or Dog{age} => age
            case Fish{age}            => age + 100
            case _                    => -1
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("x"), 3);
}

// ---------------------------------------------------------------------------
// OrPatternErrorTest — compile-time errors for invalid or-patterns
// ---------------------------------------------------------------------------

class OrPatternErrorTest : public ::testing::Test {};

// Mismatched binding names across alternatives.
TEST_F(OrPatternErrorTest, MismatchedBindingNames) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum AB { A(x) B(z) }
        match A(1) {
            case A(x) or B(z) => x
            case _            => 0
        };
    )"),
              InterpretResult::COMPILE_ERROR);
}

// Binding pattern cannot appear in an or-alternative.
TEST_F(OrPatternErrorTest, BindingPatternInOr) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum AB { A(x) }
        match A(1) {
            case v or A(x) => 0
            case _         => 0
        };
    )"),
              InterpretResult::COMPILE_ERROR);
}

// Wildcard cannot appear in an or-alternative.
TEST_F(OrPatternErrorTest, WildcardInOr) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum AB { A(x) }
        match A(1) {
            case _ or A(x) => 0
        };
    )"),
              InterpretResult::COMPILE_ERROR);
}
