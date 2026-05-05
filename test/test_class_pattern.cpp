#include "test_harness.h"
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

// ---------------------------------------------------------------------------
// ClassPatternTest — basic named-field class pattern matching
// ---------------------------------------------------------------------------

class ClassPatternTest : public ::testing::Test {};

TEST_F(ClassPatternTest, BasicNamedFieldPattern) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Point { init(x, y) { this.x = x; this.y = y; } }
        var p = Point(3, 4);
        var px = 0;
        var py = 0;
        match p {
            case Point{x, y} => { px = x; py = y }
            case _           => 0
        };
    )"),
              InterpretResult::OK);
    auto xv = h.getGlobal("px");
    auto yv = h.getGlobal("py");
    ASSERT_TRUE(xv.has_value());
    ASSERT_TRUE(yv.has_value());
    expect_num(*xv, 3.0);
    expect_num(*yv, 4.0);
}

TEST_F(ClassPatternTest, ZeroFieldClassPattern) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Sentinel { init() {} }
        var s = Sentinel();
        var got = match s {
            case Sentinel => 1
            case _        => 2
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1.0);
}

TEST_F(ClassPatternTest, GuardOnClassArm) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box { init(v) { this.v = v; } }
        var b = Box(10);
        var got = match b {
            case Box{v} if v > 5 => 1
            case Box{v}          => 2
            case _               => 3
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1.0);
}

TEST_F(ClassPatternTest, GuardFailsFallsThrough) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box { init(v) { this.v = v; } }
        var b = Box(3);
        var got = match b {
            case Box{v} if v > 5 => 1
            case Box{v}          => 2
            case _               => 3
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 2.0);
}

TEST_F(ClassPatternTest, WildcardCatchesNonInstance) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Point { init(x, y) { this.x = x; this.y = y; } }
        var got = match 42 {
            case Point{x, y} => 1
            case _           => 2
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 2.0);
}

TEST_F(ClassPatternTest, NoMatchRaisesMatchError) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Foo { init() {} }
        class Bar { init() {} }
        var f = Foo();
        match f {
            case Bar => 0
        };
    )"),
              InterpretResult::RUNTIME_ERROR);
}

TEST_F(ClassPatternTest, MultipleClassArms) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Circle { init(r)    { this.r = r; } }
        class Rect   { init(w, h) { this.w = w; this.h = h; } }
        var c = Circle(5);
        var r = Rect(4, 6);
        var ca = match c {
            case Circle{r}  => r
            case Rect{w, h} => w * h
            case _          => 0
        };
        var ra = match r {
            case Circle{r}  => r
            case Rect{w, h} => w * h
            case _          => 0
        };
    )"),
              InterpretResult::OK);
    auto cav = h.getGlobal("ca");
    auto rav = h.getGlobal("ra");
    ASSERT_TRUE(cav.has_value());
    ASSERT_TRUE(rav.has_value());
    expect_num(*cav, 5.0);
    expect_num(*rav, 24.0);
}

TEST_F(ClassPatternTest, PositionalClassPatternIsError) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Point { init(x, y) { this.x = x; this.y = y; } }
        var p = Point(1, 2);
        match p {
            case Point(x, y) => 0
            case _ => 0
        };
    )"),
              InterpretResult::COMPILE_ERROR);
}

// ---------------------------------------------------------------------------
// SubclassPatternTest — instanceof walks superclass chain
// ---------------------------------------------------------------------------

class SubclassPatternTest : public ::testing::Test {};

TEST_F(SubclassPatternTest, SubclassMatchesParentPattern) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Animal { init(name) { this.name = name; } }
        class Dog < Animal { init(name) { super.init(name); } }
        var d = Dog("Rex");
        var got = match d {
            case Animal{name} => name
            case _            => "miss"
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_str(*v, "Rex");
}

TEST_F(SubclassPatternTest, ExactClassMatchAlsoWorks) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Animal { init(name) { this.name = name; } }
        class Dog < Animal { init(name) { super.init(name); } }
        var d = Dog("Rex");
        var got = match d {
            case Dog{name} => name
            case _         => "miss"
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_str(*v, "Rex");
}

TEST_F(SubclassPatternTest, SubclassDoesNotMatchSiblingPattern) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Animal { init() {} }
        class Dog < Animal { init() { super.init(); } }
        class Cat < Animal { init() { super.init(); } }
        var d = Dog();
        var got = match d {
            case Cat => 1
            case Dog => 2
            case _   => 3
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 2.0);
}

TEST_F(SubclassPatternTest, DeepInheritanceChain) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class A { init(v) { this.v = v; } }
        class B < A { init(v) { super.init(v); } }
        class C < B { init(v) { super.init(v); } }
        var c = C(99);
        var got = match c {
            case A{v} => v
            case _    => -1
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 99.0);
}

// ---------------------------------------------------------------------------
// EnumClassCompositionTest — enum and class patterns in same program
// ---------------------------------------------------------------------------

class EnumClassCompositionTest : public ::testing::Test {};

TEST_F(EnumClassCompositionTest, EnumWrapsClassValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Result { Ok(v) Err(m) }
        class NetworkError { init(code) { this.code = code; } }
        var r = Ok(NetworkError(404));
        var got = match r {
            case Ok(v) => match v {
                case NetworkError{code} => code
                case _                 => -1
            }
            case Err(m) => -2
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 404.0);
}

TEST_F(EnumClassCompositionTest, ClassPatternNoExhaustivenessError) {
    VMTestHarness h;
    // Class patterns never trigger exhaustiveness — only enum patterns do.
    // A match with only a class arm (no wildcard) is valid at compile time;
    // MatchError is raised at runtime if no arm matches.
    ASSERT_EQ(h.run(R"(
        class Foo { init() {} }
        var f = Foo();
        match f {
            case Foo => 0
        };
    )"),
              InterpretResult::OK);
}
