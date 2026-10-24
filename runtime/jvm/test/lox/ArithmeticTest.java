package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

public final class ArithmeticTest {
    public static void main(String[] args) {
        checkEquals(3.0, LoxOps.add(1.0, 2.0), "add(1,2)");
        checkEquals("ab", LoxOps.add("a", "b"), "add strings concatenates");
        checkEquals(1.0, LoxOps.subtract(3.0, 2.0), "subtract");
        checkEquals(6.0, LoxOps.multiply(2.0, 3.0), "multiply");
        checkEquals(2.0, LoxOps.divide(6.0, 3.0), "divide");
        checkEquals(-5.0, LoxOps.negate(5.0), "negate");
        checkEquals(1.0, LoxOps.modulo(7.0, 3.0), "modulo positive");
        // Floor-division sign: result takes the sign of the divisor.
        checkEquals(2.0, LoxOps.modulo(-7.0, 3.0), "modulo negative dividend");
        checkEquals(-2.0, LoxOps.modulo(7.0, -3.0), "modulo negative divisor");

        checkThrows(() -> LoxOps.add("a", 1.0), LoxError.class, "add string+number");
        checkThrows(() -> LoxOps.subtract("a", 1.0), LoxError.class, "subtract non-number");
        checkThrows(() -> LoxOps.negate("a"), LoxError.class, "negate non-number");
        checkThrows(() -> LoxOps.checkNumber(true), LoxError.class, "checkNumber rejects bool");

        check(Boolean.TRUE.equals(LoxOps.not(false)), "not(false) == true");
        check(Boolean.FALSE.equals(LoxOps.not(true)), "not(true) == false");
        check(Boolean.TRUE.equals(LoxOps.not(null)), "not(nil) == true");
        check(Boolean.FALSE.equals(LoxOps.not(0.0)), "not(0) == false (0 is truthy)");

        check(LoxOps.isFalsy(null), "nil is falsy");
        check(LoxOps.isFalsy(false), "false is falsy");
        check(!LoxOps.isFalsy(0.0), "0 is truthy");
        check(!LoxOps.isFalsy(""), "empty string is truthy");

        check(LoxOps.equal(1.0, 1.0), "equal numbers");
        check(!LoxOps.equal(1.0, 2.0), "unequal numbers");
        check(!LoxOps.equal(0.0, "0"), "number never equals string");
        check(!LoxOps.equal(null, false), "nil never equals false");
        check(LoxOps.equal(null, null), "nil equals nil");
        check(LoxOps.equal("ab", new String(new char[] {'a', 'b'})), "strings equal by content, not identity");
        check(!LoxOps.equal(new LoxList(), new LoxList()), "two empty lists are not equal (identity)");
        LoxList sharedList = new LoxList();
        check(LoxOps.equal(sharedList, sharedList), "a list equals itself");
        check(Double.isNaN((Double) LoxOps.add(Double.NaN, 0.0)), "NaN propagates through add");
        check(!LoxOps.equal(Double.NaN, Double.NaN), "NaN != NaN");

        check(LoxOps.greater(2.0, 1.0), "greater");
        check(LoxOps.less(1.0, 2.0), "less");
        checkThrows(() -> LoxOps.greater("a", 1.0), LoxError.class, "greater on non-number");

        // Int (java.lang.Long) arithmetic (spec/04-semantics.md, Operand Kinds).
        checkEquals(Long.valueOf(3), LoxOps.add(1L, 2L), "add Ints");
        checkEquals(Long.valueOf(Long.MIN_VALUE), LoxOps.add(Long.MAX_VALUE, 1L), "an Int sum wraps");
        checkEquals(Long.valueOf(-4), LoxOps.divide(-7L, 2L), "Int divide floors");
        checkEquals(Long.valueOf(Long.MIN_VALUE), LoxOps.divide(Long.MIN_VALUE, -1L),
                "the smallest Int / -1i wraps to itself");
        checkEquals(Long.valueOf(1), LoxOps.modulo(-7L, 2L), "Int modulo floors");
        checkEquals(Long.valueOf(0), LoxOps.modulo(Long.MIN_VALUE, -1L),
                "the smallest Int % -1i is 0");
        checkEquals(Long.valueOf(Long.MIN_VALUE), LoxOps.negate(Long.MIN_VALUE),
                "negating the smallest Int wraps");
        checkThrows(() -> LoxOps.divide(1L, 0L), LoxError.class, "Int divide by zero");
        checkThrows(() -> LoxOps.add(1L, 1.0), LoxError.class, "an Int plus a Number faults");
        checkThrows(() -> LoxOps.subtract(1L, 1.0), LoxError.class, "an Int minus a Number faults");
        checkThrows(() -> LoxOps.greater(1L, 1.0), LoxError.class, "an Int vs a Number faults");
        check(!LoxOps.equal(1L, 1.0), "an Int never equals a Number");
        check(LoxOps.equal(9007199254740993L, 9007199254740993L),
                "equal Ints compare by value outside the box cache");

        // The bit operators (spec/04-semantics.md, Bit Operators).
        checkEquals(Long.valueOf(0x1C), LoxOps.bitAnd(0x1FL, 0x1CL), "bitAnd");
        checkEquals(Long.valueOf(0x1F), LoxOps.bitOr(0x10L, 0x0FL), "bitOr");
        checkEquals(Long.valueOf(0x3), LoxOps.bitXor(0x1L, 0x2L), "bitXor");
        checkEquals(Long.valueOf(-1), LoxOps.bitNot(0L), "bitNot(0i) is -1i");
        checkEquals(Long.valueOf(Long.MIN_VALUE), LoxOps.shiftLeft(1L, 63L), "1i << 63i wraps");
        checkEquals(Long.valueOf(-4), LoxOps.shiftRight(-8L, 1L), "shiftRight copies the sign bit");
        checkEquals(Long.valueOf(1), LoxOps.shiftRightUnsigned(-1L, 63L), "shiftRightUnsigned fills with zeros");
        checkThrows(() -> LoxOps.shiftLeft(1L, 64L), LoxError.class, "a shift count above 63 fault");
        checkThrows(() -> LoxOps.shiftLeft(1L, 1.0), LoxError.class, "a Number shift count faults");
        checkThrows(() -> LoxOps.bitAnd(1L, 1.0), LoxError.class, "a bit operator on a Number faults");

        System.exit(TestSupport.finish("ArithmeticTest"));
    }
}
