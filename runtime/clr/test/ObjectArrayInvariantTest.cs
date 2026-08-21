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
        object instance = LoxOps.Call(box, System.Array.Empty<object>());
        CheckNotBareObjectArray(t, instance, "constructing a class");

        return t.Finish("ObjectArrayInvariantTest");
    }
}
