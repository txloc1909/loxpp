using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text;

[assembly: InternalsVisibleTo("LoxRuntimeTests")]

namespace Lox;

/// <summary>
/// Builds a fresh <see cref="LoxGlobals"/> populated with the full native
/// stdlib surface.
///
/// <para><b>Byte boundary rule:</b> a Lox++ string is a byte sequence
/// (spec/03-types.md), not text. Every point where a string crosses into or
/// out of this process - stdout here, stdin here, file I/O in
/// <see cref="LoxFile"/> - must read or write it as <see cref="Charset"/>
/// (Latin1: the one built-in .NET encoding that maps every byte 0-255 to
/// one char and back losslessly, with no extra codepage package). The
/// process's default console encoding on Linux is UTF-8, which re-encodes
/// any byte 0x80-0xFF into two bytes and turns an unpaired high byte from
/// stdin into U+FFFD. The bytecode decoder that reads string constants out
/// of the chunk must decode them with this same charset, or <c>len()</c>
/// and <c>s[i]</c> will drift from the native VM on any non-ASCII byte.
/// </para>
/// </summary>
public static class LoxRuntime {
    public static readonly Encoding Charset = Encoding.Latin1;

    // Buffered stdout: PRINT fires millions of times in the self-hosted
    // interpreter, so an unbuffered stream would dominate runtime. A
    // ProcessExit hook flushes it, so generated code never has to remember
    // to.
    public static readonly StreamWriter Out =
        new(Console.OpenStandardOutput(), Charset, 1 << 16) { AutoFlush = false };

    private static readonly Stream s_stdin = Console.OpenStandardInput();

    static LoxRuntime() {
        AppDomain.CurrentDomain.ProcessExit += (_, _) => Out.Flush();
        // ProcessExit does not fire when an exception (a LoxError left
        // uncaught by generated code, say) terminates the process - only
        // UnhandledException does. Without this, every line already printed
        // before that point is lost, where the native VM keeps it (it
        // writes stdout unbuffered by comparison, via straight std::printf).
        AppDomain.CurrentDomain.UnhandledException += (_, _) => Out.Flush();
    }

    // The one LoxGlobals instance for this process (design decision A2: one
    // dynamic map, not one static field per global name - this is a
    // different concern, "where does generated code find the map"). A
    // generated top-level script holds a reference in a local, but a
    // generated closure body has no such local, so it reads this static
    // instead of threading the reference through every call. Safe because
    // exactly one Lox++ program ever runs per process here.
    private static LoxGlobals s_current;

    // Command-line arguments after the program name, forwarded by the
    // generated script's entry point before the first script statement
    // runs. Empty when the entry point received none, as in the
    // differential harness.
    private static string[] s_programArgs = Array.Empty<string>();

    /// <summary>The generated script's entry point forwards its own argv here before the script body runs.</summary>
    public static void SetProgramArgs(string[] args) {
        s_programArgs = args;
    }

    /// <summary>
    /// Reads one line as raw bytes (0-255), never decoding them as text.
    /// Returns nil at immediate EOF, matching std::getline's failure rule
    /// (globals.cpp): a partial trailing line with no terminator still
    /// counts as a line, and only a read that captures zero bytes is nil.
    /// Internal so a test can drive it without real stdin.
    /// </summary>
    internal static string ReadByteLine(Stream input) {
        var line = new StringBuilder();
        bool sawByte = false;
        int b;
        while ((b = input.ReadByte()) != -1) {
            sawByte = true;
            if (b == '\n') {
                break;
            }
            line.Append((char)(b & 0xFF));
        }
        return sawByte ? line.ToString() : null;
    }

    public static LoxGlobals Init() {
        var globals = new LoxGlobals();
        RegisterGlobals(globals);
        RegisterMath(globals);
        s_current = globals;
        return globals;
    }

    /// <summary>The instance the script's own <see cref="Init"/> call built. Null before that call runs.</summary>
    public static LoxGlobals Current() => s_current;

    private static void RegisterGlobals(LoxGlobals globals) {
        // clock()'s epoch is unspecified by the spec (only elapsed time
        // between two calls is meaningful), so a monotonic high-resolution
        // timestamp stands in for std::clock()'s process CPU time.
        double ticksToSeconds = 1.0 / Stopwatch.Frequency;
        globals.Define("clock", new LoxNative("clock", 0, args => Stopwatch.GetTimestamp() * ticksToSeconds));
        globals.Define("input", new LoxNative("input", 0, args => ReadByteLine(s_stdin)));
        globals.Define("str", new LoxNative("str", 1, args => LoxOps.Stringify(args[0])));
        globals.Define("len", new LoxNative("len", 1, args => {
            object v = args[0];
            if (v is LoxList list) {
                return (double)list.Elements.Count;
            }
            if (v is string s) {
                return (double)s.Length;
            }
            if (v is LoxMap map) {
                return (double)map.Size();
            }
            throw new LoxError("len() argument must be a list, string, or map.");
        }));
        globals.Define("open", new LoxNative("open", 2, args => {
            if (args[0] is not string path || args[1] is not string mode) {
                throw new LoxError("open() requires string path and mode.");
            }
            return LoxFile.Open(path, mode);
        }));
        RegisterOsAccess(globals);
    }

    // Mirrors src/stdlib/os_api.cpp: args, env, exit, time, sleep, and the
    // file-system predicates exists()/is_dir()/is_file()/stat(). Kept here
    // so a program using them does not DIVERGE in the differential suite
    // against the native VM.
    private static void RegisterOsAccess(LoxGlobals globals) {
        globals.Define("args", new LoxNative("args", 0, unused => {
            var list = new LoxList();
            foreach (string s in s_programArgs) {
                list.Elements.Add(s);
            }
            return list;
        }));
        globals.Define("env", new LoxNative("env", 1, args => {
            if (args[0] is not string name) {
                throw new LoxError("Expected a string argument.");
            }
            return Environment.GetEnvironmentVariable(name); // null -> Lox nil, matching getenv(3)
        }));
        globals.Define("exit", new LoxNative("exit", 1, args => {
            if (args[0] is not double raw) {
                throw new LoxError("exit() code must be a number.");
            }
            // Truncate toward zero, the C-style integral conversion
            // (os_api.cpp). Reject a value the cast below cannot
            // represent, mirroring the native VM's finite/int-range guard.
            if (double.IsNaN(raw) || double.IsInfinity(raw) || raw > int.MaxValue || raw < int.MinValue) {
                throw new LoxError("exit() code must be a finite number in the integer range.");
            }
            Out.Flush(); // ProcessExit does not fire for Environment.Exit on every platform path
            Environment.Exit((int)raw); // range-guarded above, so the cast truncates toward zero, matching C
            throw new InvalidOperationException("Environment.Exit must not return");
        }));
        globals.Define("time", new LoxNative("time", 0, unused => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0));
        globals.Define("sleep", new LoxNative("sleep", 1, args => {
            if (args[0] is not double seconds) {
                throw new LoxError("sleep() duration must be a number.");
            }
            if (seconds > 0) {
                System.Threading.Thread.Sleep(TimeSpan.FromSeconds(seconds));
            }
            return null;
        }));
        globals.Define("exists", new LoxNative("exists", 1, args => {
            string p = RequirePathArg(args[0]);
            return File.Exists(p) || Directory.Exists(p);
        }));
        globals.Define("is_dir", new LoxNative("is_dir", 1, args => Directory.Exists(RequirePathArg(args[0]))));
        globals.Define("is_file", new LoxNative("is_file", 1, args => File.Exists(RequirePathArg(args[0]))));
        globals.Define("stat", new LoxNative("stat", 1, args => {
            string p = RequirePathArg(args[0]);
            bool isDir = Directory.Exists(p);
            bool isFile = File.Exists(p);
            if (!isDir && !isFile) {
                return null; // Lox nil, matching os_api.cpp's statNative
            }
            var map = new LoxMap();
            map.Put("exists", true);
            map.Put("is_dir", isDir);
            map.Put("is_file", isFile);
            if (isFile) {
                try {
                    map.Put("size", (double)new FileInfo(p).Length);
                } catch (IOException) {
                    // size key omitted, matching statNative when file_size errs
                }
            }
            try {
                DateTime mtimeUtc = File.GetLastWriteTimeUtc(p);
                map.Put("mtime", new DateTimeOffset(mtimeUtc, TimeSpan.Zero).ToUnixTimeMilliseconds() / 1000.0);
            } catch (IOException) {
                // mtime key omitted, matching statNative when last_write_time errs
            }
            return map;
        }));
    }

    private static string RequirePathArg(object v) {
        if (v is not string s) {
            throw new LoxError("Expected a string argument.");
        }
        return s;
    }

    // `math` is a plain instance with native-function fields (not
    // methods), exactly like ObjInstance in math_module.cpp -
    // GET_PROPERTY/INVOKE already handle a callable field with no extra
    // machinery.
    private static void RegisterMath(LoxGlobals globals) {
        var math = new LoxInstance(new LoxClass("Math", null));
        math.Fields["abs"] = MathUnary("abs", Math.Abs);
        math.Fields["ceil"] = MathUnary("ceil", Math.Ceiling);
        math.Fields["floor"] = MathUnary("floor", Math.Floor);
        math.Fields["round"] = MathUnary("round", RoundHalfAwayFromZero);
        math.Fields["sqrt"] = MathUnary("sqrt", Math.Sqrt);
        math.Fields["cbrt"] = MathUnary("cbrt", Math.Cbrt);
        math.Fields["exp"] = MathUnary("exp", Math.Exp);
        math.Fields["log"] = MathUnary("log", Math.Log);
        math.Fields["log2"] = MathUnary("log2", Math.Log2);
        math.Fields["log10"] = MathUnary("log10", Math.Log10);
        math.Fields["sin"] = MathUnary("sin", Math.Sin);
        math.Fields["cos"] = MathUnary("cos", Math.Cos);
        math.Fields["tan"] = MathUnary("tan", Math.Tan);
        math.Fields["asin"] = MathUnary("asin", Math.Asin);
        math.Fields["acos"] = MathUnary("acos", Math.Acos);
        math.Fields["atan"] = MathUnary("atan", Math.Atan);
        math.Fields["pow"] = MathBinary("pow", Math.Pow);
        math.Fields["atan2"] = MathBinary("atan2", Math.Atan2);
        math.Fields["hypot"] = MathBinary("hypot", Hypot);
        math.Fields["min"] = MathBinary("min", Fmin);
        math.Fields["max"] = MathBinary("max", Fmax);
        math.Fields["pi"] = Math.PI;
        math.Fields["e"] = Math.E;
        math.Fields["inf"] = double.PositiveInfinity;
        // Not double.NaN: its bit pattern is 0xFFF8000000000000 (sign bit
        // set), so FormatNumber prints "-nan". src/math.cpp defines this
        // constant with std::numeric_limits<double>::quiet_NaN(), bits
        // 0x7FF8000000000000 (sign bit clear), which prints "nan".
        math.Fields["nan"] = BitConverter.Int64BitsToDouble(0x7FF8000000000000);
        globals.Define("math", math);
    }

    private static LoxNative MathUnary(string name, Func<double, double> f) {
        return new LoxNative(name, 1, args => {
            if (args[0] is not double x) {
                throw new LoxError("math function argument must be a number.");
            }
            return f(x);
        });
    }

    private static LoxNative MathBinary(string name, Func<double, double, double> f) {
        return new LoxNative(name, 2, args => {
            if (args[0] is not double x || args[1] is not double y) {
                throw new LoxError("math function arguments must be numbers.");
            }
            return f(x, y);
        });
    }

    // std::round rounds half away from zero; Math.Round(double) defaults to
    // round-half-to-even (Math.Round(-0.5) == 0, but C's round(-0.5) == -1.0).
    // Math.Round(x, MidpointRounding.AwayFromZero) matches std::round
    // exactly; Math.Abs(x) + 0.5 (the previous formula here) does not: the
    // addition itself rounds to the nearest representable double before
    // Math.Floor runs, tipping ties like 0.49999999999999994 and every odd
    // integer at or above 2^52 up by one where std::round leaves them alone.
    private static double RoundHalfAwayFromZero(double x) {
        return Math.Round(x, MidpointRounding.AwayFromZero);
    }

    // std::hypot (glibc) gives +infinity when either operand is infinite,
    // even if the other is NaN, and otherwise returns a NaN OPERAND
    // unchanged when neither is infinite. double.Hypot already matches
    // the first rule (confirmed: Hypot(PositiveInfinity, NaN) is
    // Infinity), so this only intercepts the second: a finite/NaN or
    // NaN/NaN call, where double.Hypot instead returns the CLR's own
    // canonical NaN and loses the input's sign bit.
    private static double Hypot(double x, double y) {
        if (double.IsInfinity(x) || double.IsInfinity(y)) {
            return double.Hypot(x, y);
        }
        if (double.IsNaN(x)) {
            return x;
        }
        if (double.IsNaN(y)) {
            return y;
        }
        return double.Hypot(x, y);
    }

    // std::fmin/fmax ignore a NaN operand when the other is a number, and
    // on a tie (including the +0.0/-0.0 tie) return the LEFT operand
    // unchanged. Math.Min/Max instead propagate NaN, and each has its own
    // documented zero-sign preference (Min favors -0.0, Max favors +0.0)
    // that does not depend on operand order, so a tied call is handled
    // before either BCL method ever runs.
    private static double Fmin(double a, double b) {
        if (double.IsNaN(a)) {
            return b;
        }
        if (double.IsNaN(b)) {
            return a;
        }
        if (a == b) {
            return a;
        }
        return a < b ? a : b;
    }

    private static double Fmax(double a, double b) {
        if (double.IsNaN(a)) {
            return b;
        }
        if (double.IsNaN(b)) {
            return a;
        }
        if (a == b) {
            return a;
        }
        return a > b ? a : b;
    }
}
