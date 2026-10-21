#include "test_harness.h"
#include "objects.h"
#include "value.h"

#include <gtest/gtest.h>

#include <string>

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
        enum Msg { Ping }
        var x = Msg.Ping();
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
        enum Shape { Circle Rect Triangle }
        var c = Shape.Circle();
        var r = Shape.Rect();
        var t = Shape.Triangle();
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
        enum Pair { Pair(first, second) }
        var p = Pair.Pair(10, 20);
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
    ASSERT_EQ(h.run(R"(
        enum X { A }
        enum X { B }
    )"),
              InterpretResult::COMPILE_ERROR);
}

TEST_F(EnumDeclarationTest, DuplicateCtorNameIsError) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum X { A A }
    )"),
              InterpretResult::COMPILE_ERROR);
}

TEST_F(EnumDeclarationTest, WrongArityAtRuntime) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { Pair(a, b) }
        E.Pair(1);
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
        enum Result { Ok(value) Err(msg) }
        var r = Result.Ok(42);
        var got = 0;
        match r {
            case Result.Ok(v) => got = v
            case Result.Err(m) => got = -1
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42.0);
}

TEST_F(MatchEnumTest, NamedFieldBinding) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Point { Point(x, y) }
        var p = Point.Point(3, 4);
        var px = 0;
        var py = 0;
        match p {
            case Point.Point{x, y} => { px = x; py = y }
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

TEST_F(MatchEnumTest, ZeroFieldConstructor) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Opt { Some(v) None }
        var x = Opt.None();
        var got = 0;
        match x {
            case Opt.Some(v) => got = v
            case Opt.None => got = 99
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 99.0);
}

TEST_F(MatchEnumTest, WildcardAfterConstructors) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Color { Red Green Blue }
        var c = Color.Blue();
        var got = 0;
        match c {
            case Color.Red => got = 1
            case Color.Green => got = 2
            case _ => got = 99
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 99.0);
}

TEST_F(MatchEnumTest, GuardOnConstructorArm) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum R { Ok(v) Err(m) }
        var r = R.Ok(5);
        var got = 0;
        match r {
            case R.Ok(v) if v > 3 => got = 1
            case R.Ok(v) => got = 2
            case R.Err(m) => got = 3
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1.0);
}

TEST_F(MatchEnumTest, GuardFailsFallsThrough) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum R { Ok(v) Err(m) }
        var r = R.Ok(1);
        var got = 0;
        match r {
            case R.Ok(v) if v > 3 => got = 1
            case R.Ok(v) => got = 2
            case R.Err(m) => got = 3
        };
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
        enum Coin { Heads Tails }
        var c = Coin.Heads();
        var got = 0;
        match c {
            case Coin.Heads => got = 1
            case Coin.Tails => got = 2
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1.0);
}

TEST_F(ExhaustivenessTest, MissingArmIsCompileError) {
    VMTestHarness h;
    // Missing 'Blue' arm — should fail at compile time.
    ASSERT_EQ(h.run(R"(
        enum Color { Red Green Blue }
        var c = Color.Red();
        match c {
            case Color.Red => 1
            case Color.Green => 2
        };
    )"),
              InterpretResult::COMPILE_ERROR);
}

TEST_F(ExhaustivenessTest, WildcardSuppressesExhaustivenessError) {
    VMTestHarness h;
    // c is Blue — not covered by Red arm, so the wildcard arm must be hit.
    ASSERT_EQ(h.run(R"(
        enum Color { Red Green Blue }
        var c = Color.Blue();
        var got = 0;
        match c {
            case Color.Red => got = 1
            case _ => got = 99
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 99.0);
}

// ---------------------------------------------------------------------------
// MatchGCTest — GC safety with enum values on the stack
// ---------------------------------------------------------------------------

class MatchGCTest : public ::testing::Test {};

TEST_F(MatchGCTest, EnumValueSurvivesGC) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Tree { Leaf(val) Node(left, right) }
        var t = Tree.Node(Tree.Leaf(1), Tree.Leaf(2));
        var got = 0;
        match t {
            case Tree.Leaf(v) => got = v
            case Tree.Node(l, r) => {
                match l {
                    case Tree.Leaf(v) => got = v
                    case Tree.Node(a, b) => got = -1
                }
            }
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 1.0);
}

TEST_F(MatchGCTest, ConstructorIsFirstClassValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Wrap { Box(v) }
        var ctor = Wrap.Box;
        var w = ctor(42);
        var got = 0;
        match w {
            case Wrap.Box(v) => got = v
        };
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("got");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 42.0);
}

// JUMP_TABLE holds its entry count in one byte, so a match with one arm for
// each of 256 constructors must not use a jump table.
TEST_F(MatchEnumTest, MatchOverAllTagsDoesNotUseAJumpTable) {
    std::string ctors;
    std::string arms;
    for (int i = 0; i < 256; ++i) {
        ctors += " C" + std::to_string(i);
        arms += " case E.C" + std::to_string(i) + " => " + std::to_string(i);
    }
    VMTestHarness h;
    ASSERT_EQ(h.run("enum E {" + ctors + " }\n" +
                    "var last = match E.C255() {" + arms + " };\n" +
                    "var first = match E.C0() {" + arms + " };\n"),
              InterpretResult::OK);
    auto last = h.getGlobal("last");
    auto first = h.getGlobal("first");
    ASSERT_TRUE(last.has_value());
    ASSERT_TRUE(first.has_value());
    expect_num(*last, 255.0);
    expect_num(*first, 0.0);
}
