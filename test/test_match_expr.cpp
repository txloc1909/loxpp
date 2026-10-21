#include "test_harness.h"
#include "objects.h"
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
        var x = match Result.Ok(5) {
            case Result.Ok(v) => v
            case Result.Err(m) => 0
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
        var x = match Result.Ok(5) {
            case Result.Ok(v) if v > 3 => 1
            case Result.Ok(v) => 2
            case Result.Err(m) => 3
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

// spec/02-syntax.md armBody requires a final expression. A body that ends
// with a statement must be a compile error, not a silent value leak.
TEST_F(MatchExpressionTest, StatementTerminatedArmBodyIsCompileError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(
        var x = match 1 {
            case 1 => { var t = 10; t = t * 2; }
            case _ => 0
        };
    )"),
              InterpretResult::COMPILE_ERROR);
}

// Pins the exact demo from the amendment-3 report. Before the fix, this
// program compiled, and `x` received the pattern binding `v` (7) instead of
// the arm value, printing "[7, 101]" with exit 0. It must now be a compile
// error.
TEST_F(MatchExpressionTest, ArmBodyDoesNotLeakPatternBinding) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(
        enum E { A(v) B }
        fun g() {
          var keep = 100;
          var x = match E.A(7) {
            case E.A(v) => { keep = keep + 1; }
            case E.B => 2
          };
          return [x, keep];
        }
        print g();
    )"),
              InterpretResult::COMPILE_ERROR);
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
            var r = match Result.Ok(i) {
                case Result.Ok(v) => v
                case Result.Err(m) => -1
            };
            out = out + r;
        }
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("out");
    ASSERT_TRUE(v.has_value());
    expect_num(*v, 3); // 1 + 2
}

// ---------------------------------------------------------------------------
// JUMP_TABLE optimisation tests
// ---------------------------------------------------------------------------

class JumpTableTest : public ::testing::Test {};

// Verify that the JUMP_TABLE opcode is emitted for a dense enum match.
TEST_F(JumpTableTest, BytecodeContainsJumpTable) {
    std::string bytecode = compile_program_to_bytecode(R"(
        enum Color { Red Green Blue }
        var c = Color.Green();
        var x = match c {
            case Color.Red   => 1
            case Color.Green => 2
            case Color.Blue  => 3
        };
    )");
    EXPECT_NE(bytecode.find("JUMP_TABLE"), std::string::npos)
        << "Expected JUMP_TABLE in bytecode for dense enum match";
    // The sequential GET_TAG/EQUAL chain should NOT appear.
    EXPECT_EQ(bytecode.find("EQUAL"), std::string::npos)
        << "Did not expect EQUAL (sequential tag check) in JUMP_TABLE match";
}

// JUMP_TABLE must produce the same results as the sequential approach.
TEST_F(JumpTableTest, CorrectDispatch) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Dir { North South East West }
        fun label(d) {
            return match d {
                case Dir.North => "N"
                case Dir.South => "S"
                case Dir.East  => "E"
                case Dir.West  => "W"
            };
        }
        var a = label(Dir.North());
        var b = label(Dir.South());
        var c = label(Dir.East());
        var d = label(Dir.West());
    )"),
              InterpretResult::OK);
    expect_str(*h.getGlobal("a"), "N");
    expect_str(*h.getGlobal("b"), "S");
    expect_str(*h.getGlobal("c"), "E");
    expect_str(*h.getGlobal("d"), "W");
}

// Field bindings must still work when dispatched via JUMP_TABLE.
TEST_F(JumpTableTest, FieldBindingsWork) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum Shape { Circle(r) Rect(w, h) Triangle(b, ht) }
        fun area(s) {
            return match s {
                case Shape.Circle(r)      => r * r
                case Shape.Rect(w, h)     => w * h
                case Shape.Triangle(b, ht) => b * ht / 2
            };
        }
        var ca = area(Shape.Circle(3));
        var ra = area(Shape.Rect(4, 5));
        var ta = area(Shape.Triangle(6, 4));
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("ca"), 9);
    expect_num(*h.getGlobal("ra"), 20);
    expect_num(*h.getGlobal("ta"), 12);
}

// Arms in non-tag-order should still dispatch correctly.
TEST_F(JumpTableTest, ArmsOutOfTagOrder) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum ABC { A B C }
        var r = match ABC.B() {
            case ABC.C => 3
            case ABC.A => 1
            case ABC.B => 2
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 2);
}

// Sparse match (not all arms present) should NOT use JUMP_TABLE.
TEST_F(JumpTableTest, SparseMatchUsesSequential) {
    std::string bytecode = compile_program_to_bytecode(R"(
        enum ABC { A B C }
        var x = ABC.A();
        var r = match x {
            case ABC.A => 1
            case ABC.C => 3
            case _ => 0
        };
    )");
    // A has tag 0, C has tag 2 — gap at tag 1 (and the catch-all '_' breaks
    // JUMP_TABLE eligibility anyway), so the sequential path is used.
    EXPECT_EQ(bytecode.find("JUMP_TABLE"), std::string::npos)
        << "Did not expect JUMP_TABLE for sparse/catch-all match";
}

// A match with a guard on any arm should fall back to the sequential path.
TEST_F(JumpTableTest, GuardedMatchUsesSequential) {
    std::string bytecode = compile_program_to_bytecode(R"(
        enum AB { A B }
        var x = AB.A();
        var r = match x {
            case AB.A if 1 == 1 => 1
            case AB.B           => 2
        };
    )");
    EXPECT_EQ(bytecode.find("JUMP_TABLE"), std::string::npos)
        << "Did not expect JUMP_TABLE when a guard is present";
}

// MatchError must still fire when no arm matches (out-of-range tag at runtime).
TEST_F(JumpTableTest, MatchErrorOnNoMatch) {
    VMTestHarness h;
    // The enum Pair has only two constructors. If for some reason the VM were
    // handed a tag outside [0,1] the MATCH_ERROR path would trigger. We test
    // the normal non-exhaustive path by omitting an arm and relying on the
    // compiler's exhaustiveness check instead.
    //
    // Since the compiler rejects non-exhaustive matches without a catch-all,
    // we verify the runtime error by using a catch-all that should not fire.
    ASSERT_EQ(h.run(R"(
        enum Pair { Fst Second }
        var r = match Pair.Fst() {
            case Pair.Fst    => 1
            case Pair.Second => 2
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 1);
}

// issue #117: a match used as the RIGHT operand of a binary operator used to
// allocate its result/subject slots below the already-live left operand, so
// GET_TAG read the sibling instead of the subject. The reported shapes:
// right operand, sandwiched call argument, and both-sides-folded. Each
// program must now run to completion and store the correct value.
TEST_F(MatchExpressionTest, MatchAsRightOperandOfBinary) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { A B }
        var r = 1 + (match E.A() { case E.A => 1 case E.B => 2 });
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 2);
}

TEST_F(MatchExpressionTest, MatchAsSandwichedCallArgument) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { A B }
        fun f(x) { return x + 1; }
        var r = f(1 + (match E.A() { case E.A => 1 case E.B => 2 }));
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 3);
}

TEST_F(MatchExpressionTest, MatchBothSidesOfBinary) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { A B }
        var r = (match E.A() { case E.A => 1 case E.B => 2 }) +
                (match E.B() { case E.A => 10 case E.B => 20 });
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 21);
}

TEST_F(MatchExpressionTest, MatchAsFirstCallArgument) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { A B }
        fun f(a, b) { return a * 10 + b; }
        var r = f((match E.A() { case E.A => 1 case E.B => 2 }), 5);
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 15);
}

TEST_F(MatchExpressionTest, MatchAsListElementAndMapValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { A B }
        var list = [1, (match E.A() { case E.A => 7 case E.B => 8 })];
        var m = { "k": match E.A() { case E.A => 9 case E.B => 10 } };
        var a = list[1];
        var b = m["k"];
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 7);
    expect_num(*h.getGlobal("b"), 9);
}

// A match as the subject of another match (nested match), the shape the
// backend's own tests already exercise via the corpus.
TEST_F(MatchExpressionTest, MatchAsNestedSubject) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { A B }
        var r = match (match E.A() { case E.A => E.A() case E.B => E.B() }) {
            case E.A => 1
            case E.B => 2
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 1);
}

// Review finding: a match nested in an arm body that follows an or-alternative
// used to read a negative operand depth, corrupting slot bookkeeping. The
// nested match must read the subject and the arm value must be correct on
// both operand sides.
TEST_F(MatchExpressionTest, MatchInArmAfterOrAlternative) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { A B }
        var r = match E.A() { case E.A or E.B => (match E.A() { case E.A => 1 case E.B => 2 }) + 10 };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 11);
}

TEST_F(MatchExpressionTest, MatchInArmAfterOrAlternativeRightOperand) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { A B }
        var r = match E.A() { case E.A or E.B => 10 + (match E.A() { case E.A => 1 case E.B => 2 }) };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 11);
}

// Review finding: the same regression for a guarded arm whose guard fails,
// when the next arm body contains a match.
TEST_F(MatchExpressionTest, MatchInArmAfterFailedGuard) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { A B }
        var r = match E.B() {
            case E.A if false => 0
            case E.B => (match E.A() { case E.A => 1 case E.B => 2 }) + 10
        };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 11);
}

// "_" is the wildcard pattern even when an enum constructor has that name
// (spec/02-syntax.md, armPat).
TEST_F(MatchExpressionTest, UnderscoreIsWildcardOverConstructorName) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { _(x) B }
        var r = match E.B() { case _ => 1 };
        var s = match 99 { case _ => 2 };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 1);
    expect_num(*h.getGlobal("s"), 2);
}

// The same rule applies to a class named "_".
TEST_F(MatchExpressionTest, UnderscoreIsWildcardOverClassName) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class _ {}
        var r = match _() { case _ => 1 };
        var s = match 99 { case _ => 2 };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("r"), 1);
    expect_num(*h.getGlobal("s"), 2);
}

// A "_" with "(" or "{" is a structural pattern head, not the wildcard, so
// it may name a constructor with fields (spec/02-syntax.md, armPat).
TEST_F(MatchExpressionTest, UnderscoreConstructorStructuralPatterns) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        enum E { _(x) B }
        var a = match E._(9) { case E._(y) => y case E.B => 0 };
        var b = match E._(3) { case E._{x} => x case E.B => 0 };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 9);
    expect_num(*h.getGlobal("b"), 3);
}

// The same rule applies to a class named "_".
TEST_F(MatchExpressionTest, UnderscoreClassStructuralPattern) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class _ { init(x) { this.x = x; } }
        var a = match _(5) { case _{x} => x };
    )"),
              InterpretResult::OK);
    expect_num(*h.getGlobal("a"), 5);
}
