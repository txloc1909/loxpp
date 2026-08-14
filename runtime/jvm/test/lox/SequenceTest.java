package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

public final class SequenceTest {
    private static LoxList listOf(Object... items) {
        LoxList list = new LoxList();
        for (Object item : items) {
            list.elements.add(item);
        }
        return list;
    }

    public static void main(String[] args) {
        LoxList nums = listOf(1.0, 2.0, 3.0);
        check(LoxOps.in(1.0, nums), "in(1, [1,2,3])");
        check(!LoxOps.in(4.0, nums), "in(4, [1,2,3]) is false");
        check(LoxOps.in("ell", "hello"), "in('ell','hello')");
        check(!LoxOps.in("xyz", "hello"), "in('xyz','hello') is false");
        checkThrows(() -> LoxOps.in(1.0, "hello"), LoxError.class, "in on a string requires a string lhs");
        checkThrows(() -> LoxOps.in(1.0, true), LoxError.class, "in on a non-sequence rhs");

        LoxMap m = new LoxMap();
        m.put("k", 1.0);
        check(LoxOps.in("k", m), "in('k', map)");
        check(!LoxOps.in("missing", m), "in('missing', map) is false");
        checkThrows(() -> LoxOps.in(Double.NaN, m), LoxError.class, "in rejects NaN map key");

        checkEquals("el", LoxOps.slice("hello", 1.0, 3.0), "slice('hello',1,3)");
        checkEquals("", LoxOps.slice("hello", 3.0, 1.0), "slice with start>=end is empty");
        checkEquals("hello", LoxOps.slice("hello", 0.0, 100.0), "slice clamps out-of-range end");
        LoxList sliced = (LoxList) LoxOps.slice(nums, 1.0, 3.0);
        checkEquals(2, sliced.elements.size(), "slice(list) size");
        checkEquals(2.0, sliced.elements.get(0), "slice(list) first element");
        checkThrows(() -> LoxOps.slice("hello", -1.0, 2.0), LoxError.class, "slice rejects negative index");
        checkThrows(() -> LoxOps.slice("hello", 1.5, 2.0), LoxError.class, "slice rejects non-integer index");
        checkThrows(() -> LoxOps.slice(true, 0.0, 1.0), LoxError.class, "slice requires list or string");

        checkEquals(2.0, LoxOps.getIndex(nums, 1.0), "getIndex(list,1)");
        checkEquals("e", LoxOps.getIndex("hello", 1.0), "getIndex(string,1)");
        checkEquals(1.0, LoxOps.getIndex(m, "k"), "getIndex(map,'k')");
        checkEquals(null, LoxOps.getIndex(m, "missing"), "getIndex(map, missing key) is nil");
        checkThrows(() -> LoxOps.getIndex(nums, 10.0), LoxError.class, "getIndex out of bounds");
        checkThrows(() -> LoxOps.getIndex(nums, 0.5), LoxError.class, "getIndex non-integer");
        checkThrows(() -> LoxOps.getIndex(true, 0.0), LoxError.class, "getIndex on non-sequence");

        LoxEnumCtor ctor = new LoxEnumCtor(0, 2, "Pair", "E");
        LoxEnum enumVal = (LoxEnum) ctor.call(new Object[] {10.0, 20.0});
        checkEquals(10.0, LoxOps.getIndex(enumVal, 0.0), "getIndex(enum payload, 0)");
        checkThrows(() -> LoxOps.getIndex(enumVal, 5.0), LoxError.class, "getIndex(enum) out of range");

        checkEquals(42.0, LoxOps.setIndex(nums, 0.0, 42.0), "setIndex returns the assigned value");
        checkEquals(42.0, nums.elements.get(0), "setIndex mutates the list");
        checkEquals(9.0, LoxOps.setIndex(m, "k", 9.0), "setIndex(map) returns the assigned value");
        checkEquals(9.0, m.get("k"), "setIndex(map) mutates the map");
        checkThrows(() -> LoxOps.setIndex("hello", 0.0, "H"), LoxError.class, "setIndex on a string is an error");
        checkThrows(() -> LoxOps.setIndex(nums, 99.0, 1.0), LoxError.class, "setIndex out of bounds");

        LoxIterator it = LoxOps.getIter(listOf(1.0, 2.0));
        check(it.hasNext(), "iterator has first element");
        checkEquals(1.0, it.next(), "iterator first element");
        check(it.hasNext(), "iterator has second element");
        checkEquals(2.0, it.next(), "iterator second element");
        check(!it.hasNext(), "iterator exhausted");
        checkThrows(() -> LoxOps.getIter(true), LoxError.class, "getIter on a non-iterable");

        check(LoxOps.isSeq(listOf()), "list is a sequence");
        check(LoxOps.isSeq("s"), "string is a sequence");
        check(!LoxOps.isSeq(new LoxMap()), "map is not IS_SEQ (matches Op::IS_SEQ, not the spec protocol)");
        check(!LoxOps.isSeq(1.0), "number is not a sequence");

        checkThrows(LoxOps::matchError, LoxError.class, "matchError always throws");

        System.exit(TestSupport.finish("SequenceTest"));
    }
}
