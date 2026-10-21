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
        var a = match Msg.Quit() {
            case Msg.Quit or Msg.Pause => 1
            case Msg.Resume        => 2
        };
        var b = match Msg.Pause() {
            case Msg.Quit or Msg.Pause => 1
            case Msg.Resume        => 2
        };
        var c = match Msg.Resume() {
            case Msg.Quit or Msg.Pause => 1
            case Msg.Resume        => 2
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
        var a = match Ev.Move(10) {
            case Ev.Move(x) or Ev.Teleport(x) => x
            case Ev.Stop                   => 0
        };
        var b = match Ev.Teleport(42) {
            case Ev.Move(x) or Ev.Teleport(x) => x
            case Ev.Stop                   => 0
        };
        var c = match Ev.Stop() {
            case Ev.Move(x) or Ev.Teleport(x) => x
            case Ev.Stop                   => 0
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
        var a = match Color.Red() {
            case Color.Red or Color.Green or Color.Blue => 1
            case Color.Alpha                => 2
        };
        var b = match Color.Green() {
            case Color.Red or Color.Green or Color.Blue => 1
            case Color.Alpha                => 2
        };
        var c = match Color.Blue() {
            case Color.Red or Color.Green or Color.Blue => 1
            case Color.Alpha                => 2
        };
        var d = match Color.Alpha() {
            case Color.Red or Color.Green or Color.Blue => 1
            case Color.Alpha                => 2
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
        var a = match Ev.Move(5) {
            case Ev.Move(x) or Ev.Teleport(x) if x > 3 => 1
            case Ev.Move(x) or Ev.Teleport(x)           => 2
            case Ev.Stop                              => 3
        };
        var b = match Ev.Teleport(1) {
            case Ev.Move(x) or Ev.Teleport(x) if x > 3 => 1
            case Ev.Move(x) or Ev.Teleport(x)           => 2
            case Ev.Stop                              => 3
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
        var x = match Shape.Circle() {
            case Shape.Circle or Shape.Rect => 1
            case Shape.Triangle       => 2
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
        var a = match Shape.Circle(7) {
            case Shape.Circle{r} or Shape.Square{r} => r
            case Shape.Rect{w}                => w
        };
        var b = match Shape.Square(3) {
            case Shape.Circle{r} or Shape.Square{r} => r
            case Shape.Rect{w}                => w
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
        match AB.A(1) {
            case AB.A(x) or AB.B(z) => x
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
        match AB.A(1) {
            case v or AB.A(x) => 0
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
        match AB.A(1) {
            case _ or AB.A(x) => 0
        };
    )"),
              InterpretResult::COMPILE_ERROR);
}
