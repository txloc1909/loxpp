using Lox;

namespace LoxRuntimeTests;

public static class SequenceTest {
    private static LoxList ListOf(params object[] items) {
        var list = new LoxList();
        foreach (object item in items) {
            list.Elements.Add(item);
        }
        return list;
    }

    public static int Run() {
        var t = new TestSupport();

        LoxList built = LoxOps.BuildList(new object[] { 1.0, 2.0, 3.0 });
        t.CheckEquals(3, built.Elements.Count, "buildList size");
        t.CheckEquals(1.0, built.Elements[0], "buildList keeps first-to-last order");
        t.CheckEquals(3.0, built.Elements[2], "buildList keeps first-to-last order (last)");
        t.CheckEquals(0, LoxOps.BuildList(System.Array.Empty<object>()).Elements.Count, "buildList of zero elements");

        LoxMap builtMap = LoxOps.BuildMap(new object[] { "a", 1.0, "b", 2.0 });
        t.CheckEquals(2, builtMap.Size(), "buildMap size");
        t.CheckEquals(1.0, builtMap.Get("a"), "buildMap keeps key0/val0");
        t.CheckEquals(2.0, builtMap.Get("b"), "buildMap keeps key1/val1");
        t.CheckEquals(0, LoxOps.BuildMap(System.Array.Empty<object>()).Size(), "buildMap of zero pairs");
        // A later pair overwrites an earlier one under the same key - same
        // first-to-last write order as vm.cpp's own BUILD_MAP loop.
        t.CheckEquals(9.0, LoxOps.BuildMap(new object[] { "k", 1.0, "k", 9.0 }).Get("k"),
            "buildMap: later pair wins on a repeated key");
        t.CheckThrows(() => LoxOps.BuildMap(new object[] { double.NaN, 1.0 }), typeof(LoxError),
            "buildMap rejects NaN as a key");

        LoxList nums = ListOf(1.0, 2.0, 3.0);
        t.Check(LoxOps.In(1.0, nums), "in(1, [1,2,3])");
        t.Check(!LoxOps.In(4.0, nums), "in(4, [1,2,3]) is false");
        t.Check(LoxOps.In("ell", "hello"), "in('ell','hello')");
        t.Check(!LoxOps.In("xyz", "hello"), "in('xyz','hello') is false");
        t.CheckThrows(() => LoxOps.In(1.0, "hello"), typeof(LoxError), "in on a string requires a string lhs");
        t.CheckThrows(() => LoxOps.In(1.0, true), typeof(LoxError), "in on a non-sequence rhs");

        var m = new LoxMap();
        m.Put("k", 1.0);
        t.Check(LoxOps.In("k", m), "in('k', map)");
        t.Check(!LoxOps.In("missing", m), "in('missing', map) is false");
        t.CheckThrows(() => LoxOps.In(double.NaN, m), typeof(LoxError), "in rejects NaN map key");

        t.CheckEquals("el", LoxOps.Slice("hello", 1.0, 3.0), "slice('hello',1,3)");
        t.CheckEquals("", LoxOps.Slice("hello", 3.0, 1.0), "slice with start>=end is empty");
        t.CheckEquals("hello", LoxOps.Slice("hello", 0.0, 100.0), "slice clamps out-of-range end");
        var sliced = (LoxList)LoxOps.Slice(nums, 1.0, 3.0);
        t.CheckEquals(2, sliced.Elements.Count, "slice(list) size");
        t.CheckEquals(2.0, sliced.Elements[0], "slice(list) first element");
        t.CheckThrows(() => LoxOps.Slice("hello", -1.0, 2.0), typeof(LoxError), "slice rejects negative index");
        t.CheckThrows(() => LoxOps.Slice("hello", 1.5, 2.0), typeof(LoxError), "slice rejects non-integer index");
        t.CheckThrows(() => LoxOps.Slice(true, 0.0, 1.0), typeof(LoxError), "slice requires list or string");

        t.CheckEquals(2.0, LoxOps.GetIndex(nums, 1.0), "getIndex(list,1)");
        t.CheckEquals("e", LoxOps.GetIndex("hello", 1.0), "getIndex(string,1)");
        t.CheckEquals(1.0, LoxOps.GetIndex(m, "k"), "getIndex(map,'k')");
        t.CheckEquals(null, LoxOps.GetIndex(m, "missing"), "getIndex(map, missing key) is nil");
        t.CheckThrows(() => LoxOps.GetIndex(nums, 10.0), typeof(LoxError), "getIndex out of bounds");
        t.CheckThrows(() => LoxOps.GetIndex(nums, 0.5), typeof(LoxError), "getIndex non-integer");
        t.CheckThrows(() => LoxOps.GetIndex(true, 0.0), typeof(LoxError), "getIndex on non-sequence");

        var ctor = new LoxEnumCtor(0, 2, "Pair", "E");
        var enumVal = (LoxEnum)ctor.Call(new object[] { 10.0, 20.0 });
        t.CheckEquals(10.0, LoxOps.GetIndex(enumVal, 0.0), "getIndex(enum payload, 0)");
        t.CheckThrows(() => LoxOps.GetIndex(enumVal, 5.0), typeof(LoxError), "getIndex(enum) out of range");

        t.CheckEquals(42.0, LoxOps.SetIndex(nums, 0.0, 42.0), "setIndex returns the assigned value");
        t.CheckEquals(42.0, nums.Elements[0], "setIndex mutates the list");
        t.CheckEquals(9.0, LoxOps.SetIndex(m, "k", 9.0), "setIndex(map) returns the assigned value");
        t.CheckEquals(9.0, m.Get("k"), "setIndex(map) mutates the map");
        t.CheckThrows(() => LoxOps.SetIndex("hello", 0.0, "H"), typeof(LoxError), "setIndex on a string is an error");
        t.CheckThrows(() => LoxOps.SetIndex(nums, 99.0, 1.0), typeof(LoxError), "setIndex out of bounds");

        LoxIterator it = LoxOps.GetIter(ListOf(1.0, 2.0));
        t.Check(it.HasNext(), "iterator has first element");
        t.CheckEquals(1.0, it.Next(), "iterator first element");
        t.Check(it.HasNext(), "iterator has second element");
        t.CheckEquals(2.0, it.Next(), "iterator second element");
        t.Check(!it.HasNext(), "iterator exhausted");
        t.CheckThrows(() => LoxOps.GetIter(true), typeof(LoxError), "getIter on a non-iterable");

        // ITER_HAS_NEXT/ITER_NEXT's own opcode-level wrappers, over the raw
        // object the generated CIL actually holds.
        object iterAsObject = LoxOps.GetIter(ListOf(5.0, 6.0));
        t.Check(LoxOps.IterHasNext(iterAsObject), "iterHasNext wrapper, first element");
        t.CheckEquals(5.0, LoxOps.IterNext(iterAsObject), "iterNext wrapper, first element");
        t.Check(LoxOps.IterHasNext(iterAsObject), "iterHasNext wrapper, second element");
        t.CheckEquals(6.0, LoxOps.IterNext(iterAsObject), "iterNext wrapper, second element");
        t.Check(!LoxOps.IterHasNext(iterAsObject), "iterHasNext wrapper, exhausted");

        t.Check(LoxOps.IsSeq(ListOf()), "list is a sequence");
        t.Check(LoxOps.IsSeq("s"), "string is a sequence");
        t.Check(!LoxOps.IsSeq(new LoxMap()), "map is not IS_SEQ (matches Op::IS_SEQ, not the spec protocol)");
        t.Check(!LoxOps.IsSeq(1.0), "number is not a sequence");

        t.CheckEquals("MatchError: no matching arm.", LoxOps.MatchError().Message,
            "matchError builds a LoxError with the vm.cpp message");

        return t.Finish("SequenceTest");
    }
}
