using System;
using System.IO;
using Lox;

namespace LoxRuntimeTests;

public static class RuntimeStdlibTest {
    private static object Call(LoxGlobals g, string name, params object[] args) {
        return ((ILoxCallable)g.Get(name)).Call(args);
    }

    private static object CallField(LoxInstance instance, string name, params object[] args) {
        return ((ILoxCallable)instance.Fields[name]).Call(args);
    }

    private static LoxList ListOf(params object[] items) {
        var list = new LoxList();
        foreach (object item in items) {
            list.Elements.Add(item);
        }
        return list;
    }

    public static int Run() {
        var t = new TestSupport();
        LoxGlobals globals = LoxRuntime.Init();

        t.Check(Call(globals, "clock") is double, "clock() returns a number");
        CheckClockIsProcessorTime(t, globals);
        t.CheckEquals("42", Call(globals, "str", 42.0), "str(42)");
        t.CheckEquals("true", Call(globals, "str", true), "str(true)");
        t.CheckEquals(3.0, Call(globals, "len", ListOf(1.0, 2.0, 3.0)), "len(list)");
        t.CheckEquals(5.0, Call(globals, "len", "hello"), "len(string)");
        t.CheckThrows(() => Call(globals, "len", true), typeof(LoxError), "len() rejects a non-sequence");

        object math = globals.Get("math");
        t.Check(math is LoxInstance, "math is an instance");
        t.CheckEquals(4.0, CallField((LoxInstance)math, "abs", -4.0), "math.abs(-4)");
        t.CheckEquals(2.0, CallField((LoxInstance)math, "sqrt", 4.0), "math.sqrt(4)");
        t.CheckEquals(-1.0, CallField((LoxInstance)math, "round", -0.5), "math.round(-0.5) rounds away from zero");
        t.CheckEquals(1.0, CallField((LoxInstance)math, "round", 0.5), "math.round(0.5) rounds away from zero");
        // Both guard against Math.Abs(x) + 0.5 rounding to a tie before
        // Math.Floor runs, which tips these two cases one too high.
        t.CheckEquals(0.0, CallField((LoxInstance)math, "round", 0.49999999999999994),
            "math.round(0.49999999999999994) is not a true tie, so it stays 0");
        double justAbove2Pow52 = 4503599627370497.0; // 2^52 + 1: already integral, spacing is 1 here
        t.CheckEquals(justAbove2Pow52, CallField((LoxInstance)math, "round", justAbove2Pow52),
            "math.round leaves an already-integral value at 2^52+1 unchanged");
        t.CheckEquals(5.0, ((ILoxCallable)((LoxInstance)math).Fields["min"])
            .Call(new object[] { double.NaN, 5.0 }), "math.min ignores a NaN operand");
        t.Check((double)((LoxInstance)math).Fields["pi"] > 3.14, "math.pi is present");
        // Bits, not value: double.IsNaN is true for both signs of NaN, but
        // FormatNumber reads the sign bit, so the bit pattern is what must
        // match src/math.cpp's std::numeric_limits<double>::quiet_NaN().
        t.CheckEquals(0x7FF8000000000000L, BitConverter.DoubleToInt64Bits((double)((LoxInstance)math).Fields["nan"]),
            "math.nan has the same bit pattern as C's quiet_NaN(), not .NET's negative-signed double.NaN");

        CheckOpenRoundtrip(t, globals);
        CheckOsAccess(t, globals);

        return t.Finish("RuntimeStdlibTest");
    }

    private static void CheckOsAccess(TestSupport t, LoxGlobals globals) {
        // args() empty until setProgramArgs is called (the generated entry
        // point forwards its own argv during real runs).
        object noArgs = Call(globals, "args");
        t.Check(noArgs is LoxList na && na.Elements.Count == 0, "args() is empty by default");
        LoxRuntime.SetProgramArgs(new[] { "alpha", "beta" });
        var args = (LoxList)Call(globals, "args");
        t.CheckEquals(2, args.Elements.Count, "args() reflects setProgramArgs");
        t.CheckEquals("alpha", args.Elements[0], "args()[0]");
        t.CheckEquals("beta", args.Elements[1], "args()[1]");

        // env(): present for a real variable, nil for an unknown name.
        t.Check(Environment.GetEnvironmentVariable("PATH") != null
            ? Call(globals, "env", "PATH") != null
            : true, "env() reads the environment");
        t.Check(Call(globals, "env", "LOXPP_OS_API_NO_SUCH_VAR_XYZ") == null, "env() returns nil for an unknown name");
        t.CheckThrows(() => Call(globals, "env", 42.0), typeof(LoxError), "env() rejects a non-string");

        // exit(): the terminating path cannot be asserted in-process; the
        // rejection paths can be.
        t.CheckThrows(() => Call(globals, "exit", "no"), typeof(LoxError), "exit() rejects a non-number");
        t.CheckThrows(() => Call(globals, "exit", 1e300), typeof(LoxError), "exit() rejects an out-of-range code");
        t.CheckThrows(() => Call(globals, "exit", double.NaN), typeof(LoxError), "exit() rejects NaN");

        // time() is wall-clock seconds since the Unix epoch.
        var now = (double)Call(globals, "time");
        t.Check(now > 0, "time() is positive");

        // sleep() returns nil and accepts a fraction.
        t.Check(Call(globals, "sleep", 0.0) == null, "sleep(0) returns nil");
        t.CheckThrows(() => Call(globals, "sleep", "long"), typeof(LoxError), "sleep() rejects a non-number");

        CheckFsOsAccess(t, globals);
    }

    private static void CheckFsOsAccess(TestSupport t, LoxGlobals globals) {
        string tmpDir = Path.Combine(Path.GetTempPath(), $"lox-rt-os-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tmpDir);
        try {
            string tmpFile = Path.Combine(tmpDir, "note.txt");
            File.WriteAllBytes(tmpFile, LoxRuntime.Charset.GetBytes("hello loxpp"));

            t.Check(true.Equals(Call(globals, "exists", tmpFile)), "exists() true for a file");
            t.Check(true.Equals(Call(globals, "exists", tmpDir)), "exists() true for a directory");
            t.Check(false.Equals(Call(globals, "exists", tmpDir + "/nope.lox")),
                "exists() false for a missing path");

            t.Check(true.Equals(Call(globals, "is_dir", tmpDir)), "is_dir() true for a directory");
            t.Check(false.Equals(Call(globals, "is_dir", tmpFile)), "is_dir() false for a file");
            t.Check(true.Equals(Call(globals, "is_file", tmpFile)), "is_file() true for a file");
            t.Check(false.Equals(Call(globals, "is_file", tmpDir)), "is_file() false for a directory");

            t.CheckThrows(() => Call(globals, "exists", 1.0), typeof(LoxError), "exists() rejects a non-string");
            t.CheckThrows(() => Call(globals, "is_dir", 1.0), typeof(LoxError), "is_dir() rejects a non-string");
            t.CheckThrows(() => Call(globals, "is_file", 1.0), typeof(LoxError), "is_file() rejects a non-string");

            object missing = Call(globals, "stat", tmpDir + "/nope.lox");
            t.Check(missing == null, "stat() of a missing path returns nil");

            var fileStat = (LoxMap)Call(globals, "stat", tmpFile);
            t.Check(true.Equals(fileStat.Get("exists")), "stat(file).exists");
            t.Check(true.Equals(fileStat.Get("is_file")), "stat(file).is_file");
            t.Check(false.Equals(fileStat.Get("is_dir")), "stat(file).is_dir");
            t.CheckEquals(11.0, fileStat.Get("size"), "stat(file).size");
            t.Check(fileStat.Get("mtime") is double mtime1 && mtime1 > 0, "stat(file).mtime is positive");

            var dirStat = (LoxMap)Call(globals, "stat", tmpDir);
            t.Check(true.Equals(dirStat.Get("exists")), "stat(dir).exists");
            t.Check(true.Equals(dirStat.Get("is_dir")), "stat(dir).is_dir");
            t.Check(dirStat.Get("size") == null, "stat(dir) has no size key");

            t.CheckThrows(() => Call(globals, "stat", 1.0), typeof(LoxError), "stat() rejects a non-string");
        } finally {
            Directory.Delete(tmpDir, recursive: true);
        }
    }

    /// <summary>
    /// spec/05-stdlib.md: clock() is processor time, not wall-clock time -
    /// only its epoch is unspecified. A pure wall-clock stand-in would
    /// advance by nearly the full sleep duration below with no CPU work
    /// done; real processor time must not.
    /// </summary>
    private static void CheckClockIsProcessorTime(TestSupport t, LoxGlobals globals) {
        double beforeBusy = (double)Call(globals, "clock");
        var busy = System.Diagnostics.Stopwatch.StartNew();
        double sink = 0;
        while (busy.Elapsed.TotalMilliseconds < 30) {
            sink += Math.Sqrt(sink + 1);
        }
        double afterBusy = (double)Call(globals, "clock");
        t.Check(afterBusy > beforeBusy, "clock() advances while the CPU is actually busy");

        double beforeSleep = (double)Call(globals, "clock");
        System.Threading.Thread.Sleep(300);
        double afterSleep = (double)Call(globals, "clock");
        t.Check(afterSleep - beforeSleep < 0.2,
            $"clock() must not advance by anywhere near a 300ms sleep's wall-clock duration " +
            $"(advanced by {afterSleep - beforeSleep:F3}s) - it measures processor time, matching " +
            "std::clock(), not wall-clock time");
    }

    private static void CheckOpenRoundtrip(TestSupport t, LoxGlobals globals) {
        string path = Path.Combine(Path.GetTempPath(), $"lox-rt-stdlib-open-{Guid.NewGuid():N}.txt");
        try {
            object file = Call(globals, "open", path, "w");
            t.Check(file is LoxFile, "open() returns a LoxFile");
            ((LoxFile)file).Close();
            t.CheckThrows(() => Call(globals, "open", 1.0, "w"), typeof(LoxError),
                "open() requires string arguments");
        } finally {
            File.Delete(path);
        }
    }
}
