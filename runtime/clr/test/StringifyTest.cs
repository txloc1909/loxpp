using System.Globalization;
using System.IO;
using System.Threading;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// A fixed set of number cases, each captured from build/loxpp itself (the
/// %g oracle) via a probe run inside the build container.
/// LoxOps.Stringify must match byte for byte. StringifyDifferentialTest
/// covers the same claim over a much larger generated set, run against the
/// native binary directly rather than a captured list.
/// </summary>
public static class StringifyTest {
    public static int Run() {
        var t = new TestSupport();

        t.CheckEquals("0", LoxOps.Stringify(0.0), "stringify(0)");
        t.CheckEquals("1", LoxOps.Stringify(1.0), "stringify(1)");
        t.CheckEquals("-1", LoxOps.Stringify(-1.0), "stringify(-1)");
        t.CheckEquals("3", LoxOps.Stringify(3.0), "stringify(3)");
        t.CheckEquals("3.5", LoxOps.Stringify(3.5), "stringify(3.5)");
        t.CheckEquals("0.1", LoxOps.Stringify(0.1), "stringify(0.1)");
        t.CheckEquals("0.5", LoxOps.Stringify(0.5), "stringify(0.5)");
        t.CheckEquals("100", LoxOps.Stringify(100.0), "stringify(100)");
        t.CheckEquals("1e+06", LoxOps.Stringify(1000000.0), "stringify(1000000)");
        t.CheckEquals("999999", LoxOps.Stringify(999999.0), "stringify(999999)");
        t.CheckEquals("1.23457e+06", LoxOps.Stringify(1234567.0), "stringify(1234567)");
        t.CheckEquals("0.0001", LoxOps.Stringify(0.0001), "stringify(0.0001)");
        t.CheckEquals("1e-05", LoxOps.Stringify(0.00001), "stringify(0.00001)");
        t.CheckEquals("1e+20", LoxOps.Stringify(System.Math.Pow(10, 20)), "stringify(1e20)");
        t.CheckEquals("1e-20", LoxOps.Stringify(System.Math.Pow(10, -20)), "stringify(1e-20)");
        t.CheckEquals("-0", LoxOps.Stringify(-0.0), "stringify(-0.0)");
        // A runtime (not compile-time-folded) division, exactly as
        // LoxOps.Divide always performs it: on x86_64 this carries a set
        // sign bit, matching glibc's "-nan" for the native oracle.
        t.CheckEquals("-nan", LoxOps.Stringify(LoxOps.Divide(0.0, 0.0)), "stringify(0/0)");
        t.CheckEquals("inf", LoxOps.Stringify(LoxOps.Divide(1.0, 0.0)), "stringify(1/0)");
        t.CheckEquals("2.5", LoxOps.Stringify(2.5), "stringify(2.5)");
        t.CheckEquals("0.333333", LoxOps.Stringify(LoxOps.Divide(1.0, 3.0)), "stringify(1/3)");
        t.CheckEquals("1.23457e+08", LoxOps.Stringify(123456789.0), "stringify(123456789)");

        // The exponent must stay ASCII digits under every culture, not
        // just the invariant one: ar-SA renders a plain "D2" format with
        // Arabic-Indic digits when no explicit CultureInfo is given.
        CultureInfo savedCulture = Thread.CurrentThread.CurrentCulture;
        try {
            Thread.CurrentThread.CurrentCulture = CultureInfo.GetCultureInfo("ar-SA");
            t.CheckEquals("1e+06", LoxOps.Stringify(1000000.0), "stringify(1e6) stays ASCII under a non-Latin culture");
        } finally {
            Thread.CurrentThread.CurrentCulture = savedCulture;
        }

        t.CheckEquals("true", LoxOps.Stringify(true), "stringify(true)");
        t.CheckEquals("false", LoxOps.Stringify(false), "stringify(false)");
        t.CheckEquals("nil", LoxOps.Stringify(null), "stringify(nil)");
        t.CheckEquals("hello", LoxOps.Stringify("hello"), "stringify(string)");

        var fn = new DelegateClosure("greet", 0, new object[0][], (self, a) => null);
        t.CheckEquals("<fn greet>", LoxOps.Stringify(fn), "stringify(named closure)");
        var script = new DelegateClosure(null, 0, new object[0][], (self, a) => null);
        t.CheckEquals("<script>", LoxOps.Stringify(script), "stringify(script closure)");
        t.CheckEquals("<native fn>", LoxOps.Stringify(new LoxNative("clock", 0, a => null)),
            "stringify(native)");

        var klass = new LoxClass("Dog", null);
        t.CheckEquals("Dog", LoxOps.Stringify(klass), "stringify(class)");
        var instance = new LoxInstance(klass);
        t.CheckEquals("Dog instance", LoxOps.Stringify(instance), "stringify(instance)");
        t.CheckEquals("<fn greet>", LoxOps.Stringify(new LoxBoundMethod(instance, fn)),
            "stringify(bound method)");

        var list = new LoxList();
        t.CheckEquals("[]", LoxOps.Stringify(list), "stringify(empty list)");
        list.Elements.Add(1.0);
        list.Elements.Add("hello");
        list.Elements.Add(true);
        list.Elements.Add(null);
        t.CheckEquals("[1, hello, true, nil]", LoxOps.Stringify(list), "stringify(list)");

        var map = new LoxMap();
        t.CheckEquals("{}", LoxOps.Stringify(map), "stringify(empty map)");
        map.Put("a", 1.0);
        map.Put("b", 2.0);
        t.CheckEquals("{a: 1, b: 2}", LoxOps.Stringify(map), "stringify(map)");

        var ctor = new LoxEnumCtor(1, 2, "Ok", "Result");
        t.CheckEquals("<ctor Result::Ok>", LoxOps.Stringify(ctor), "stringify(enum ctor)");
        t.CheckEquals("Result::Ok(1, 2)",
            LoxOps.Stringify(new LoxEnum(ctor, new object[] { 1.0, 2.0 })), "stringify(enum with payload)");
        var nullary = new LoxEnumCtor(0, 0, "None", "Option");
        t.CheckEquals("Option::None", LoxOps.Stringify(new LoxEnum(nullary, System.Array.Empty<object>())),
            "stringify(nullary enum)");

        t.CheckEquals("<iterator>", LoxOps.Stringify(LoxOps.GetIter(list)), "stringify(iterator)");

        string tmpPath = Path.Combine(Path.GetTempPath(), $"lox-rt-stringify-{System.Guid.NewGuid():N}.txt");
        try {
            LoxFile file = LoxFile.Open(tmpPath, "w");
            t.CheckEquals("<file>", LoxOps.Stringify(file), "stringify(file)");
            file.Close();
        } finally {
            File.Delete(tmpPath);
        }

        return t.Finish("StringifyTest");
    }
}
