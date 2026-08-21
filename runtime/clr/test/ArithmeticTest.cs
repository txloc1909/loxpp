using System;
using Lox;

namespace LoxRuntimeTests;

public static class ArithmeticTest {
    public static int Run() {
        var t = new TestSupport();

        // A NaN OPERAND to `%` keeps its own bit pattern through
        // LoxOps.Modulo, matching glibc's fmod (src/vm.cpp Op::MODULO):
        // confirmed against native build/loxpp, `math.nan % 3` prints
        // "nan" and `math.inf % math.nan` prints "nan" too, both keeping
        // math.nan's own (positive) sign. A domain-error NaN with NEITHER
        // operand NaN (an infinite dividend) is a different case - native
        // prints "-nan" there, and this runtime already agreed before
        // this fix, so that line is unaffected by it.
        double positiveNan = BitConverter.Int64BitsToDouble(0x7FF8000000000000);
        t.CheckEquals("nan", LoxOps.Stringify(LoxOps.Modulo(positiveNan, 3.0)),
            "a NaN dividend keeps its own sign through modulo");
        t.CheckEquals("nan", LoxOps.Stringify(LoxOps.Modulo(double.PositiveInfinity, positiveNan)),
            "a NaN divisor keeps its own sign through modulo");
        t.CheckEquals("-nan", LoxOps.Stringify(LoxOps.Modulo(double.PositiveInfinity, 3.0)),
            "an infinite dividend with no NaN operand still hits the domain-error NaN");

        t.CheckEquals(3.0, LoxOps.Add(1.0, 2.0), "add(1,2)");
        t.CheckEquals("ab", LoxOps.Add("a", "b"), "add strings concatenates");
        t.CheckEquals(1.0, LoxOps.Subtract(3.0, 2.0), "subtract");
        t.CheckEquals(6.0, LoxOps.Multiply(2.0, 3.0), "multiply");
        t.CheckEquals(2.0, LoxOps.Divide(6.0, 3.0), "divide");
        t.CheckEquals(-5.0, LoxOps.Negate(5.0), "negate");
        t.CheckEquals(1.0, LoxOps.Modulo(7.0, 3.0), "modulo positive");
        // Floor-division sign: result takes the sign of the divisor.
        t.CheckEquals(2.0, LoxOps.Modulo(-7.0, 3.0), "modulo negative dividend");
        t.CheckEquals(-2.0, LoxOps.Modulo(7.0, -3.0), "modulo negative divisor");

        t.CheckThrows(() => LoxOps.Add("a", 1.0), typeof(LoxError), "add string+number");
        t.CheckThrows(() => LoxOps.Subtract("a", 1.0), typeof(LoxError), "subtract non-number");
        t.CheckThrows(() => LoxOps.Negate("a"), typeof(LoxError), "negate non-number");
        t.CheckThrows(() => LoxOps.CheckNumber(true), typeof(LoxError), "checkNumber rejects bool");

        t.Check(true.Equals(LoxOps.Not(false)), "not(false) == true");
        t.Check(false.Equals(LoxOps.Not(true)), "not(true) == false");
        t.Check(true.Equals(LoxOps.Not(null)), "not(nil) == true");
        t.Check(false.Equals(LoxOps.Not(0.0)), "not(0) == false (0 is truthy)");

        t.Check(LoxOps.IsFalsy(null), "nil is falsy");
        t.Check(LoxOps.IsFalsy(false), "false is falsy");
        t.Check(!LoxOps.IsFalsy(0.0), "0 is truthy");
        t.Check(!LoxOps.IsFalsy(""), "empty string is truthy");

        t.Check(LoxOps.Equal(1.0, 1.0), "equal numbers");
        t.Check(!LoxOps.Equal(1.0, 2.0), "unequal numbers");
        t.Check(!LoxOps.Equal(0.0, "0"), "number never equals string");
        t.Check(!LoxOps.Equal(null, false), "nil never equals false");
        t.Check(LoxOps.Equal(null, null), "nil equals nil");
        t.Check(LoxOps.Equal("ab", new string(new[] { 'a', 'b' })), "strings equal by content, not identity");
        t.Check(!LoxOps.Equal(new LoxList(), new LoxList()), "two empty lists are not equal (identity)");
        var sharedList = new LoxList();
        t.Check(LoxOps.Equal(sharedList, sharedList), "a list equals itself");
        t.Check(double.IsNaN((double)LoxOps.Add(double.NaN, 0.0)), "NaN propagates through add");
        t.Check(!LoxOps.Equal(double.NaN, double.NaN), "NaN != NaN");

        t.Check(LoxOps.Greater(2.0, 1.0), "greater");
        t.Check(LoxOps.Less(1.0, 2.0), "less");
        t.CheckThrows(() => LoxOps.Greater("a", 1.0), typeof(LoxError), "greater on non-number");

        return t.Finish("ArithmeticTest");
    }
}
