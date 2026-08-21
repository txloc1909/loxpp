using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// The %g parity test is differential, not a fixed list: native
/// build/loxpp is the oracle. This generates a set of doubles, writes a
/// Lox++ program that prints each one as a literal, runs it on the native
/// binary, and compares each line against LoxOps.Stringify(the same
/// double) - byte for byte, or the test fails.
///
/// Every literal is written with no exponent (the scanner's own NUMBER
/// grammar has none) as the exact decimal value the double's round-trip
/// ("G17") text represents, just re-punctuated - a pure text
/// transformation, so it loses no precision. A correctly-rounded parser
/// (native's std::stod and this generator alike) therefore parses the
/// literal back to the identical double bit pattern the value started as,
/// with no separate round-trip through the CLR side needed.
///
/// <para>The generated set stays in the double's NORMAL exponent range on
/// purpose: compiler.cpp's own number literal parser calls std::stod with
/// no try/catch, and glibc's strtod sets errno=ERANGE - which std::stod
/// turns into an uncaught std::out_of_range, aborting the whole process -
/// for a literal that parses to a SUBNORMAL double, even though that
/// result is a perfectly valid double. This is a pre-existing native
/// parser limitation, unrelated to number formatting and out of this
/// runtime's scope; this generator avoids it rather than working around it
/// here.</para>
/// </summary>
public static class StringifyDifferentialTest {
    // Generous, but finite: a hang here must fail the test loudly instead
    // of hanging the whole suite (and CI) forever.
    private const int NativeTimeoutMs = 30000;

    public static int Run() {
        var t = new TestSupport();

        string nativeBin = Environment.GetEnvironmentVariable("LOXPP_BIN");
        if (string.IsNullOrEmpty(nativeBin) || !File.Exists(nativeBin)) {
            t.Check(false,
                "LOXPP_BIN is not set to an existing native loxpp binary - " +
                "run via tools/test_lox_rt_clr.sh, which builds it and exports the path");
            return t.Finish("StringifyDifferentialTest");
        }

        if (!CheckOracleFitness(t, nativeBin)) {
            return t.Finish("StringifyDifferentialTest");
        }

        List<double> values = GenerateValues();
        string scriptPath = Path.Combine(Path.GetTempPath(), $"lox-rt-stringify-diff-{Guid.NewGuid():N}.lox");
        try {
            File.WriteAllText(scriptPath, BuildScript(values), Encoding.ASCII);

            (int exitCode, string stdout, string stderr, bool timedOut) = RunNative(nativeBin, scriptPath);
            if (timedOut) {
                t.Check(false,
                    $"native run of the generated differential script did not finish within {NativeTimeoutMs}ms");
                return t.Finish("StringifyDifferentialTest");
            }
            if (exitCode != 0) {
                t.Check(false, $"native run of the generated differential script failed (exit {exitCode}): {stderr}");
                return t.Finish("StringifyDifferentialTest");
            }

            string[] lines = stdout.Split('\n');
            t.CheckEquals(values.Count + 1, lines.Length,
                "native printed exactly one line per value, plus the trailing newline's empty tail");

            for (int i = 0; i < values.Count && i < lines.Length - 1; i++) {
                double v = values[i];
                string expected = LoxOps.Stringify(v);
                string actual = lines[i];
                t.CheckEquals(expected, actual,
                    $"stringify parity for value #{i} (bits=0x{BitConverter.DoubleToInt64Bits(v):x16})");
            }
        } finally {
            File.Delete(scriptPath);
        }

        return t.Finish("StringifyDifferentialTest");
    }

    /// <summary>
    /// Proves the oracle binary is fit to compare against before spending
    /// the real (large, slow) sweep on it. A build with
    /// LOXPP_DEBUG_PRINT_CODE or LOXPP_DEBUG_TRACE_EXECUTION on - the
    /// CMake debug preset's own default - interleaves a chunk disassembly
    /// and a per-instruction stack trace into stdout alongside the
    /// program's own `print` output, so splitting stdout one line per
    /// value would silently read the wrong line instead of failing. A
    /// single known-good `print 1;` catches that build here, loudly,
    /// before the real sweep can misread it.
    /// </summary>
    private static bool CheckOracleFitness(TestSupport t, string nativeBin) {
        string probePath = Path.Combine(Path.GetTempPath(), $"lox-rt-stringify-fitness-{Guid.NewGuid():N}.lox");
        try {
            File.WriteAllText(probePath, "print 1;\n", Encoding.ASCII);
            (int exitCode, string stdout, string _, bool timedOut) = RunNative(nativeBin, probePath);
            if (timedOut) {
                t.Check(false,
                    $"the oracle binary at {nativeBin} did not finish a single `print 1;` within {NativeTimeoutMs}ms");
                return false;
            }
            bool fit = exitCode == 0 && stdout == "1\n";
            t.Check(fit,
                $"the oracle binary at {nativeBin} must print exactly \"1\" for `print 1;` and exit 0 " +
                $"(got exit {exitCode}, stdout {Summarize(stdout)}) - rebuild it with " +
                "`cmake --preset release && cmake --build build --target loxpp`, " +
                "since LOXPP_DEBUG_PRINT_CODE/LOXPP_DEBUG_TRACE_EXECUTION (the debug preset's default) " +
                "put extra disassembly and trace text on this same stream");
            return fit;
        } finally {
            File.Delete(probePath);
        }
    }

    private static string Summarize(string s) => s.Length <= 120 ? $"\"{s}\"" : $"\"{s.Substring(0, 120)}...\" ({s.Length} bytes)";

    /// <summary>
    /// Runs the native binary on <paramref name="scriptPath"/> and drains
    /// both stdout and stderr concurrently. Reading one stream fully with
    /// <see cref="System.IO.StreamReader.ReadToEnd"/> before starting the
    /// other deadlocks the moment the unread pipe fills (64 KiB on Linux):
    /// the child blocks on its next write to that pipe and never reaches
    /// the point where it closes the stream this method is waiting on. A
    /// debug build's LOXPP_DEBUG_LOG_GC output reaches that on any
    /// nontrivial script.
    /// </summary>
    private static (int ExitCode, string Stdout, string Stderr, bool TimedOut) RunNative(string nativeBin, string scriptPath) {
        var psi = new ProcessStartInfo {
            FileName = nativeBin,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            StandardOutputEncoding = LoxRuntime.Charset,
        };
        psi.ArgumentList.Add(scriptPath);

        using Process proc = Process.Start(psi);
        Task<string> stdoutTask = proc.StandardOutput.ReadToEndAsync();
        Task<string> stderrTask = proc.StandardError.ReadToEndAsync();
        if (!proc.WaitForExit(NativeTimeoutMs)) {
            try {
                proc.Kill(entireProcessTree: true);
            } catch {
                // best effort - the process may have exited between the
                // WaitForExit timeout and this Kill call
            }
            return (-1, "", "", true);
        }
        Task.WaitAll(stdoutTask, stderrTask);
        return (proc.ExitCode, stdoutTask.Result, stderrTask.Result, false);
    }

    private static string BuildScript(List<double> values) {
        var sb = new StringBuilder();
        foreach (double v in values) {
            bool negative = double.IsNegative(v);
            string magnitudeLiteral = ToFixedDecimalLiteral(Math.Abs(v));
            sb.Append("print ");
            if (negative) {
                sb.Append('-');
            }
            sb.Append(magnitudeLiteral).Append(";\n");
        }
        return sb.ToString();
    }

    /// <summary>
    /// The exact decimal value a double's round-trip ("G17") text
    /// represents, re-punctuated with no exponent - digits only, or
    /// digits with one embedded decimal point, matching Scanner::
    /// consumeNumber's grammar. Pure text surgery: shifting where the
    /// decimal point falls loses no precision, so this parses back to the
    /// identical double G17 itself would.
    /// </summary>
    private static string ToFixedDecimalLiteral(double magnitude) {
        string s = magnitude.ToString("G17", CultureInfo.InvariantCulture);
        int eIdx = s.IndexOfAny(new[] { 'e', 'E' });
        string mantissa = eIdx >= 0 ? s.Substring(0, eIdx) : s;
        int exponent = eIdx >= 0 ? int.Parse(s.Substring(eIdx + 1), CultureInfo.InvariantCulture) : 0;
        int dot = mantissa.IndexOf('.');
        string digits = dot >= 0 ? mantissa.Remove(dot, 1) : mantissa;
        int pointPos = (dot >= 0 ? dot : mantissa.Length) + exponent;

        if (pointPos <= 0) {
            return "0." + new string('0', -pointPos) + digits;
        }
        if (pointPos >= digits.Length) {
            return digits + new string('0', pointPos - digits.Length);
        }
        return digits.Substring(0, pointPos) + "." + digits.Substring(pointPos);
    }

    private static List<double> GenerateValues() {
        var values = new List<double>();

        // The named cases from the node checkpoint, explicitly.
        double[] named = {
            0.0, 1.0, -1.0, 2.0, 3.0, -3.0, 10.0, -10.0, 42.0, 100.0, -100.0,
            0.1 + 0.2,     // the canonical floating-point-imprecision case
            1e-5, 1e6, 1e21,
            1.0 / 3.0, -1.0 / 3.0,
            -0.0,
        };
        values.AddRange(named);

        // 6-and-7-significant-digit boundary values, where %g switches to
        // scientific notation and where round-half-to-even can tip either way.
        double[] boundary = {
            100000.0, 999999.0, 1000000.0, 1000001.0,
            9999995.0, 9999994.5, 1234565.0, 1234575.0,
            123456.0, 1234567.0, 12345678.0, 123456789.0,
            -999999.0, -1234567.0,
        };
        values.AddRange(boundary);

        // Very large and very small magnitudes, swept across the double's
        // normal exponent range (deliberately stopping short of the
        // subnormal range - see the class remarks on native's own stod
        // limit there).
        int[] exponents = {
            -300, -250, -200, -150, -100, -50,
            -30, -20, -10, -5, -4, -3, -2, -1, 0,
            1, 2, 3, 4, 5, 10, 20, 30, 50, 100, 150, 200, 250, 300, 308,
        };
        foreach (int k in exponents) {
            double v = Math.Pow(10.0, k);
            if (double.IsFinite(v) && v != 0.0) {
                values.Add(v);
                values.Add(-v);
            }
        }
        values.Add(double.MaxValue);
        values.Add(-double.MaxValue);
        values.Add(2.2250738585072014E-308); // smallest positive normal double

        // A seeded pseudo-random sweep of raw bit patterns across the
        // whole finite double range, for coverage beyond the hand-picked
        // cases above. Seeded so a failure is always reproducible.
        var rng = new Random(20260821);
        var buffer = new byte[8];
        int sampled = 0;
        while (sampled < 300) {
            rng.NextBytes(buffer);
            long bits = BitConverter.ToInt64(buffer, 0);
            double v = BitConverter.Int64BitsToDouble(bits);
            // Subnormal values are excluded for the same reason the exponent
            // sweep above stops short of them - see the class remarks.
            if (double.IsFinite(v) && v != 0.0 && !double.IsSubnormal(v)) {
                values.Add(v);
                sampled++;
            }
        }

        return values;
    }
}
