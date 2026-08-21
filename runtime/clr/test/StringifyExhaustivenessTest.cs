using System;
using System.Collections.Generic;
using System.Linq;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// INVARIANT: LoxOps.Stringify must have a branch for every concrete type
/// that implements ILoxCallable, because GET_PROPERTY, INVOKE, and a
/// closure's own Call can each hand one back to generated code as an
/// ordinary Lox value (a map or file method value read off an instance,
/// exactly as this once slipped through - see LoxMapMethod, LoxFileMethod).
/// A fixed per-type list only proves the types someone remembered to add,
/// so this test instead finds every concrete ILoxCallable implementor in
/// the LoxRuntime assembly by reflection: a new implementor with no probe
/// registered below fails this test immediately, before Stringify can
/// throw on it in a generated program. LoxClosure is abstract and is
/// handled in Stringify by an `is LoxClosure` check, which already covers
/// every subclass a later emission node defines, so this test does not
/// need a probe for it.
/// </summary>
public static class StringifyExhaustivenessTest {
    private static LoxMap SampleMap() {
        var map = new LoxMap();
        map.Put("a", 1.0);
        return map;
    }

    private static LoxFile SampleFile() {
        string path = System.IO.Path.Combine(System.IO.Path.GetTempPath(), $"lox-rt-exhaustive-{Guid.NewGuid():N}.txt");
        return LoxFile.Open(path, "w");
    }

    private static Dictionary<Type, object> BuildProbes() {
        var klass = new LoxClass("Probe", null);
        var closureFn = new DelegateClosure("m", 0, Array.Empty<object[]>(), (self, a) => null);
        var instance = new LoxInstance(klass);
        return new Dictionary<Type, object> {
            [typeof(LoxNative)] = new LoxNative("f", 0, a => null),
            [typeof(LoxClass)] = klass,
            [typeof(LoxBoundMethod)] = new LoxBoundMethod(instance, closureFn),
            [typeof(LoxEnumCtor)] = new LoxEnumCtor(0, 0, "Ctor", "Enum"),
            [typeof(LoxMapMethod)] = LoxOps.GetProperty(SampleMap(), "has"),
            [typeof(LoxFileMethod)] = LoxOps.GetProperty(SampleFile(), "write"),
        };
    }

    public static int Run() {
        var t = new TestSupport();
        Dictionary<Type, object> probes = BuildProbes();

        Type[] callableTypes = typeof(LoxNative).Assembly.GetTypes()
            .Where(ty => typeof(ILoxCallable).IsAssignableFrom(ty) && !ty.IsAbstract && !ty.IsInterface)
            .OrderBy(ty => ty.Name, StringComparer.Ordinal)
            .ToArray();

        t.Check(callableTypes.Length > 0, "sanity: the assembly has at least one concrete ILoxCallable implementor");

        foreach (Type ty in callableTypes) {
            bool hasProbe = probes.TryGetValue(ty, out object probe);
            t.Check(hasProbe, $"{ty.Name} needs a probe instance registered in StringifyExhaustivenessTest.BuildProbes");
            if (!hasProbe) {
                continue;
            }
            bool threw = false;
            string thrownType = null;
            try {
                LoxOps.Stringify(probe);
            } catch (Exception ex) {
                threw = true;
                thrownType = ex.GetType().Name;
            }
            t.Check(!threw, $"Stringify must not throw on a {ty.Name} value (threw {thrownType})");
        }

        return t.Finish("StringifyExhaustivenessTest");
    }
}
