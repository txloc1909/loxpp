using System.Collections.Generic;
using Lox;

namespace LoxRuntimeTests;

public static class MapTest {
    public static int Run() {
        var t = new TestSupport();

        var map = new LoxMap();
        t.CheckEquals(0, map.Size(), "new map is empty");
        map.Put(null, "nil-key"); // nil is a legal map key
        t.CheckEquals("nil-key", map.Get(null), "nil key round-trips");
        t.Check(map.Has(null), "has() finds a nil key");

        map.Put(0.0, "zero");
        t.CheckEquals("zero", map.Get(-0.0), "-0.0 and 0.0 look up the same entry");
        map.Put(-0.0, "neg-zero");
        t.CheckEquals(2, map.Size(), "storing -0.0 after 0.0 overwrites, not inserts");
        t.CheckEquals("neg-zero", map.Get(0.0), "the later write under -0.0 is what 0.0 now reads");
        // The stored value is normalized for lookup, but the key printed
        // back must be the sign the caller last wrote, matching
        // CoreHashMap::set (native `m[0]=1; m[-z]=2; print m.keys();`
        // prints `[-0]`, not `[0]`).
        foreach (var e in map.Entries()) {
            if ("neg-zero".Equals(e.Value)) {
                t.CheckEquals("-0", LoxOps.Stringify(e.Key), "a later write under -0.0 displays as -0, not 0");
            }
        }

        map.Remove(null);
        t.Check(!map.Has(null), "remove() deletes the key");
        map.Remove("never-there"); // no-op, matches ObjMap::mapDel on a missing key
        t.CheckEquals(1, map.Size(), "removing an absent key is a no-op");

        // Insertion order, not the native table's bucket order (the map
        // order gap is legal: spec/03-types.md leaves map order
        // unspecified).
        var ordered = new LoxMap();
        ordered.Put("b", 2.0);
        ordered.Put("a", 1.0);
        ordered.Put("c", 3.0);
        var keysInOrder = new List<object>();
        foreach (var e in ordered.Entries()) {
            keysInOrder.Add(e.Key);
        }
        t.CheckEquals("[b, a, c]", ListToString(keysInOrder), "iteration follows insertion order");

        object hasMethod = ordered.GetMethod("has");
        t.CheckEquals(true, ((ILoxCallable)hasMethod).Call(new object[] { "a" }), "map.has via getMethod");
        t.Check(!ReferenceEquals(hasMethod, ordered.GetMethod("has")),
            "getMethod returns a fresh object on every read, matching src/vm.cpp's per-read ObjBoundNative wrap");
        object keysMethod = ordered.GetMethod("keys");
        var keys = (LoxList)((ILoxCallable)keysMethod).Call(System.Array.Empty<object>());
        t.CheckEquals(3, keys.Elements.Count, "map.keys() returns every key");
        t.CheckEquals("[b, a, c]", ListToString(keys.Elements), "map.keys() follows insertion order too");
        object entriesMethod = ordered.GetMethod("entries");
        var entries = (LoxList)((ILoxCallable)entriesMethod).Call(System.Array.Empty<object>());
        var firstPair = (LoxList)entries.Elements[0];
        t.CheckEquals(2, firstPair.Elements.Count, "each entries() pair is [key, value]");
        t.Check(ordered.GetMethod("nonexistent") == null, "getMethod returns null for an unknown name");

        // src/vm.cpp's Op::GET_PROPERTY wraps a fresh ObjBoundNative on
        // every read, so `m1.has == m1.has` is false there, the same as
        // `m1.has == m2.has` - LoxOps.Equal's reference-identity rule
        // gives false for both.
        var otherMap = new LoxMap();
        otherMap.Put("z", 1.0);
        t.Check(!LoxOps.Equal(ordered.GetMethod("has"), ordered.GetMethod("has")),
            "two reads of the same map's 'has' method are not Lox-equal (fresh object per read)");
        t.Check(!LoxOps.Equal(ordered.GetMethod("has"), otherMap.GetMethod("has")),
            "two different maps' 'has' method values are not Lox-equal");
        t.Check(!LoxOps.Equal(ordered.GetMethod("has"), otherMap.GetMethod("keys")),
            "two different method names on maps are not Lox-equal");

        return t.Finish("MapTest");
    }

    private static string ListToString(List<object> items) {
        var sb = new System.Text.StringBuilder("[");
        for (int i = 0; i < items.Count; i++) {
            if (i > 0) {
                sb.Append(", ");
            }
            sb.Append(items[i]);
        }
        return sb.Append(']').ToString();
    }
}
