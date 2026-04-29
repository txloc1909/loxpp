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

// ---------------------------------------------------------------------------
// EnumDeclarationTest — constructor objects created in global scope
// ---------------------------------------------------------------------------

class EnumDeclarationTest : public ::testing::Test {};

TEST_F(EnumDeclarationTest, SingleConstructor) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Msg { ping }
        var x = ping();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("x");
    ASSERT_TRUE(v.has_value());
    ASSERT_TRUE(isEnumValue(*v));
    EXPECT_EQ(asObjEnum(as<Obj*>(*v))->ctor->tag, 0);
}

TEST_F(EnumDeclarationTest, MultipleConstructors) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Shape { circle rect triangle }
        var c = circle();
        var r = rect();
        var t = triangle();
    )"),
              InterpretResult::OK);
    auto cv = h.getGlobal("c");
    auto rv = h.getGlobal("r");
    auto tv = h.getGlobal("t");
    ASSERT_TRUE(cv.has_value());
    ASSERT_TRUE(rv.has_value());
    ASSERT_TRUE(tv.has_value());
    EXPECT_EQ(asObjEnum(as<Obj*>(*cv))->ctor->tag, 0);
    EXPECT_EQ(asObjEnum(as<Obj*>(*rv))->ctor->tag, 1);
    EXPECT_EQ(asObjEnum(as<Obj*>(*tv))->ctor->tag, 2);
}

TEST_F(EnumDeclarationTest, ConstructorWithFields) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Pair { pair(first, second) }
        var p = pair(10, 20);
    )"),
              InterpretResult::OK);
    auto pv = h.getGlobal("p");
    ASSERT_TRUE(pv.has_value());
    ASSERT_TRUE(isEnumValue(*pv));
    ObjEnum* e = asObjEnum(as<Obj*>(*pv));
    ASSERT_EQ(e->fields.size(), 2u);
    expect_num(e->fields[0], 10.0);
    expect_num(e->fields[1], 20.0);
}

TEST_F(EnumDeclarationTest, DuplicateEnumNameIsError) {
    VMTestHarness h;
    ASSERT_NE(h.run(R"(
        enum X { a }
        enum X { b }
    )"),
              InterpretResult::OK);
}

TEST_F(EnumDeclarationTest, DuplicateCtorNameIsError) {
    VMTestHarness h;
    ASSERT_NE(h.run(R"(
        enum X { a a }
    )"),
              InterpretResult::OK);
}

TEST_F(EnumDeclarationTest, WrongArityAtRuntime) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { pair(a, b) }
        pair(1);
    )"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// MatchEnumTest — constructor pattern matching
// ---------------------------------------------------------------------------

class MatchEnumTest : public ::testing::Test {};

TEST_F(MatchEnumTest, PositionalBinding) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Result { ok(value) err(msg) }
        var r = ok(42);
        var got = 0;
        match r {
            case ok(v) => got = v;
            case err(m) => got = -1;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42.0);
}

TEST_F(MatchEnumTest, NamedFieldBinding) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Point { point(x, y) }
        var p = point(3, 4);
        var px = 0;
        var py = 0;
        match p {
            case point{x, y} => { px = x; py = y; }
        }
    )"),
              InterpretResult::OK);
    auto xv = h.getGlobal("px");
    auto yv = h.getGlobal("py");
    ASSERT_TRUE(xv.has_value());
    ASSERT_TRUE(yv.has_value());
    expect_num(*xv, 3.0);
    expect_num(*yv, 4.0);
}

TEST_F(MatchEnumTest, ZeroFieldConstructor) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Opt { some(v) none }
        var x = none();
        var got = 0;
        match x {
            case some(v) => got = v;
            case none => got = 99;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 99.0);
}

TEST_F(MatchEnumTest, WildcardAfterConstructors) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Color { red green blue }
        var c = blue();
        var got = 0;
        match c {
            case red => got = 1;
            case green => got = 2;
            case _ => got = 99;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 99.0);
}

TEST_F(MatchEnumTest, GuardOnConstructorArm) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum R { ok(v) err(m) }
        var r = ok(5);
        var got = 0;
        match r {
            case ok(v) if v > 3 => got = 1;
            case ok(v) => got = 2;
            case err(m) => got = 3;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1.0);
}

TEST_F(MatchEnumTest, GuardFailsFallsThrough) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum R { ok(v) err(m) }
        var r = ok(1);
        var got = 0;
        match r {
            case ok(v) if v > 3 => got = 1;
            case ok(v) => got = 2;
            case err(m) => got = 3;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 2.0);
}

// ---------------------------------------------------------------------------
// ExhaustivenessTest — compile-time coverage checks
// ---------------------------------------------------------------------------

class ExhaustivenessTest : public ::testing::Test {};

TEST_F(ExhaustivenessTest, CompleteMatchPasses) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Coin { heads tails }
        var c = heads();
        var got = 0;
        match c {
            case heads => got = 1;
            case tails => got = 2;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1.0);
}

TEST_F(ExhaustivenessTest, MissingArmIsCompileError) {
    VMTestHarness h;
    // Missing 'blue' arm — should fail at compile time.
    ASSERT_NE(h.run(R"(
        enum Color { red green blue }
        var c = red();
        match c {
            case red => 1;
            case green => 2;
        }
    )"),
              InterpretResult::OK);
}

TEST_F(ExhaustivenessTest, WildcardSuppressesExhaustivenessError) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Color { red green blue }
        var c = red();
        var got = 0;
        match c {
            case red => got = 1;
            case _ => got = 99;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1.0);
}

// ---------------------------------------------------------------------------
// MatchGCTest — GC safety with enum values on the stack
// ---------------------------------------------------------------------------

class MatchGCTest : public ::testing::Test {};

TEST_F(MatchGCTest, EnumValueSurvivesGC) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Tree { leaf(val) node(left, right) }
        var t = node(leaf(1), leaf(2));
        var got = 0;
        match t {
            case leaf(v) => got = v;
            case node(l, r) => {
                match l {
                    case leaf(v) => got = v;
                    case node(a, b) => got = -1;
                }
            }
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1.0);
}

TEST_F(MatchGCTest, ConstructorIsFirstClassValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Wrap { box(v) }
        var ctor = box;
        var w = ctor(42);
        var got = 0;
        match w {
            case box(v) => got = v;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42.0);
}
