using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// Shared plumbing for a differential test that runs a Lox++ program on
/// the native release binary and compares its stdout against this
/// runtime's own computation of the same values. StringifyDifferentialTest
/// and MathDifferentialTest both depend on this exact mechanism (the pipe
/// deadlock fix, the oracle fitness probe, the literal writer); it exists
/// here once so neither carries its own copy.
/// </summary>
public static class NativeOracle {
    // Generous, but finite: a hang here must fail the test loudly instead
    // of hanging the whole suite (and CI) forever.
    public const int TimeoutMs = 30000;

    /// <summary>
    /// Reads LOXPP_BIN and confirms it names an existing, fit oracle
    /// binary. Returns the path, or null after recording exactly why not,
    /// so a caller can bail out of its own Run() the same way every other
    /// suite in this project does on a missing prerequisite.
    /// </summary>
    public static string RequireFitBinary(TestSupport t) {
        string nativeBin = Environment.GetEnvironmentVariable("LOXPP_BIN");
        if (string.IsNullOrEmpty(nativeBin) || !File.Exists(nativeBin)) {
            t.Check(false,
                "LOXPP_BIN is not set to an existing native loxpp binary - " +
                "run via tools/test_lox_rt_clr.sh, which builds it and exports the path");
            return null;
        }
        return CheckFitness(t, nativeBin) ? nativeBin : null;
    }

    /// <summary>
    /// Proves the oracle binary is fit to compare against before spending
    /// a real (large, slow) sweep on it. A build with
    /// LOXPP_DEBUG_PRINT_CODE or LOXPP_DEBUG_TRACE_EXECUTION on - the
    /// CMake debug preset's own default - interleaves a chunk disassembly
    /// and a per-instruction stack trace into stdout alongside the
    /// program's own `print` output, so splitting stdout one line per
    /// value would silently read the wrong line instead of failing. A
    /// single known-good `print 1;` catches that build here, loudly,
    /// before the real sweep can misread it.
    /// </summary>
    private static bool CheckFitness(TestSupport t, string nativeBin) {
        string probePath = Path.Combine(Path.GetTempPath(), $"lox-rt-oracle-fitness-{Guid.NewGuid():N}.lox");
        try {
            File.WriteAllText(probePath, "print 1;\n", Encoding.ASCII);
            (int exitCode, string stdout, string _, bool timedOut) = Run(nativeBin, probePath);
            if (timedOut) {
                t.Check(false, $"the oracle binary at {nativeBin} did not finish a single `print 1;` within {TimeoutMs}ms");
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

    public static string Summarize(string s) =>
        s.Length <= 120 ? $"\"{s}\"" : $"\"{s.Substring(0, 120)}...\" ({s.Length} bytes)";

    /// <summary>
    /// Runs the native binary on <paramref name="scriptPath"/> and drains
    /// both stdout and stderr concurrently. Reading one stream fully with
    /// StreamReader.ReadToEnd before starting the other deadlocks the
    /// moment the unread pipe fills (64 KiB on Linux): the child blocks on
    /// its next write to that pipe and never reaches the point where it
    /// closes the stream this method is waiting on. A debug build's
    /// LOXPP_DEBUG_LOG_GC output reaches that on any nontrivial script.
    /// </summary>
    public static (int ExitCode, string Stdout, string Stderr, bool TimedOut) Run(string nativeBin, string scriptPath) {
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
        if (!proc.WaitForExit(TimeoutMs)) {
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

    /// <summary>
    /// The exact decimal value a finite double's round-trip ("G17") text
    /// represents, re-punctuated with no exponent - digits only, or digits
    /// with one embedded decimal point, matching Scanner::consumeNumber's
    /// grammar. Pure text surgery: shifting where the decimal point falls
    /// loses no precision, so this parses back to the identical double
    /// G17 itself would. Callers negate separately (the grammar has no
    /// signed literal); pass a non-negative magnitude.
    /// </summary>
    public static string ToFixedDecimalLiteral(double magnitude) {
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
}
