using System;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// Enforces the BINDING INVARIANT documented at LoxOps.GetIndex: no Lox
/// value may be a bare object[]. The captured-local discriminator tells a
/// ref-cell from a raw value at run time with `is object[]`; a bare
/// object[] handed back as a Lox value would silently misread as a cell
/// the first time it reached a captured local slot, with no verifier
/// error and no exception.
/// </summary>
public static class ObjectArrayInvariantTest {
    private static void CheckNotBareObjectArray(TestSupport t, object value, string description) {
        t.Check(value is not object[], description + " must not be a bare object[]");
    }

    public static int Run() {
        var t = new TestSupport();

        // Prove the check itself can fail before trusting it below: an
        // actual object[] must trip CheckNotBareObjectArray.
        object bareArray = new object[] { 1.0 };
        t.Check(bareArray is object[], "sanity: a bare object[] is detected as one");

        CheckNotBareObjectArray(t, LoxOps.BuildList(new object[] { 1.0, 2.0 }), "buildList's result");
        CheckNotBareObjectArray(t, LoxOps.BuildMap(new object[] { "a", 1.0 }), "buildMap's result");

        LoxList list = LoxOps.BuildList(new object[] { "x", "y" });
        CheckNotBareObjectArray(t, LoxOps.GetIndex(list, 0.0), "getIndex on a list");

        LoxMap map = LoxOps.BuildMap(new object[] { "k", 42.0 });
        CheckNotBareObjectArray(t, LoxOps.GetIndex(map, "k"), "getIndex on a map");

        // The one legitimate object[] in this runtime (LoxEnum.Payload)
        // must stay wrapped inside its LoxEnum, never handed back bare.
        var pair = new LoxEnumCtor(1, 2, "Pair", "Tuple");
        object built = LoxOps.Call(pair, new object[] { 1.0, 2.0 });
        CheckNotBareObjectArray(t, built, "an enum constructor's result");
        t.Check(built is LoxEnum, "an enum constructor's result is a LoxEnum, not its raw payload");

        var box = new LoxClass("Box", null);
        object instance = LoxOps.Call(box, Array.Empty<object>());
        CheckNotBareObjectArray(t, instance, "constructing a class");

        LoxList sliceSrc = LoxOps.BuildList(new object[] { 1.0, 2.0, 3.0 });
        CheckNotBareObjectArray(t, LoxOps.Slice(sliceSrc, 0.0, 2.0), "Slice on a list");
        CheckNotBareObjectArray(t, LoxOps.Slice("hello", 1.0, 3.0), "Slice on a string");

        LoxList popSrc = LoxOps.BuildList(new object[] { 1.0, 2.0 });
        CheckNotBareObjectArray(t, LoxOps.Invoke(popSrc, "pop", Array.Empty<object>()), "Invoke 'pop' on a list");

        LoxMap invokeMap = LoxOps.BuildMap(new object[] { "a", 1.0, "b", 2.0 });
        CheckNotBareObjectArray(t, LoxOps.Invoke(invokeMap, "keys", Array.Empty<object>()), "Invoke 'keys' on a map");
        CheckNotBareObjectArray(t, LoxOps.Invoke(invokeMap, "values", Array.Empty<object>()), "Invoke 'values' on a map");
        CheckNotBareObjectArray(t, LoxOps.Invoke(invokeMap, "entries", Array.Empty<object>()), "Invoke 'entries' on a map");
        CheckNotBareObjectArray(t, LoxOps.GetProperty(invokeMap, "has"), "GetProperty of a map method value");

        LoxIterator listIter = LoxOps.GetIter(LoxOps.BuildList(new object[] { "x" }));
        CheckNotBareObjectArray(t, LoxOps.IterNext(listIter), "IterNext over a list");
        LoxIterator mapIter = LoxOps.GetIter(LoxOps.BuildMap(new object[] { "k", 1.0 }));
        CheckNotBareObjectArray(t, LoxOps.IterNext(mapIter), "IterNext over a map");

        var greeter = new LoxClass("Greeter", null);
        greeter.Methods["greet"] = new DelegateClosure("greet", 0, Array.Empty<object[]>(), (self, a) => "hi");
        object greeterInstance = LoxOps.Call(greeter, Array.Empty<object>());
        CheckNotBareObjectArray(t, LoxOps.GetProperty(greeterInstance, "greet"), "GetProperty of a bound instance method");

        var identity = new DelegateClosure("identity", 1, Array.Empty<object[]>(), (self, a) => a[0]);
        CheckNotBareObjectArray(t, LoxOps.Call(identity, new object[] { 1.0 }), "Call on a closure");

        return t.Finish("ObjectArrayInvariantTest");
    }
}
