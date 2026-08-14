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

        System.exit(TestSupport.finish("ArithmeticTest"));
    }
}
