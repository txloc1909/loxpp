using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// Differential parity for the whole `math` table, the same shape as
/// StringifyDifferentialTest: native build/loxpp is the oracle, run
/// through <see cref="NativeOracle"/>. Every unary and binary function on
/// `math` is discovered by walking <c>math.Fields</c> for a
/// <see cref="LoxNative"/> of that arity - not a hardcoded function-name
/// list - so a math function added to this runtime later is swept
/// automatically instead of needing a matching entry added here by hand.
///
/// <para>The probe values include math.nan, math.inf, and -math.inf on
/// purpose: a NaN or an infinite operand is exactly where a .NET math
/// method has been seen to diverge from its C/glibc counterpart (a
/// canonical, differently-signed NaN in place of the operand's own, or a
/// documented zero-sign preference where the native function returns
/// whichever operand it was given). A value set that stuck to ordinary
/// finite numbers would not exercise that boundary at all.</para>
/// </summary>
public static class MathDifferentialTest {
    public static int Run() {
        var t = new TestSupport();

        string nativeBin = NativeOracle.RequireFitBinary(t);
        if (nativeBin == null) {
            return t.Finish("MathDifferentialTest");
        }

        LoxGlobals globals = LoxRuntime.Init();
        var math = (LoxInstance)globals.Get("math");

        var unary = new List<LoxNative>();
        var binary = new List<LoxNative>();
        foreach (object field in math.Fields.Values) {
            if (field is LoxNative native) {
                if (native.Arity == 1) {
                    unary.Add(native);
                } else if (native.Arity == 2) {
                    binary.Add(native);
                }
            }
        }
        t.Check(unary.Count > 0, "sanity: math has at least one unary function to sweep");
        t.Check(binary.Count > 0, "sanity: math has at least one binary function to sweep");

        Probe[] probes = BuildProbes(math);

        var lines = new List<(string Script, object Expected)>();
        foreach (LoxNative fn in unary) {
            foreach (Probe p in probes) {
                object expected = SafeCall(fn, new object[] { p.Value });
                lines.Add(($"math.{fn.Name}({p.SourceExpr})", expected));
            }
        }
        foreach (LoxNative fn in binary) {
            foreach (Probe a in probes) {
                foreach (Probe b in probes) {
                    object expected = SafeCall(fn, new object[] { a.Value, b.Value });
                    lines.Add(($"math.{fn.Name}({a.SourceExpr}, {b.SourceExpr})", expected));
                }
            }
        }

        string scriptPath = Path.Combine(Path.GetTempPath(), $"lox-rt-math-diff-{Guid.NewGuid():N}.lox");
        try {
            var sb = new StringBuilder();
            foreach ((string expr, _) in lines) {
                sb.Append("print ").Append(expr).Append(";\n");
            }
            File.WriteAllText(scriptPath, sb.ToString(), Encoding.ASCII);

            (int exitCode, string stdout, string stderr, bool timedOut) = NativeOracle.Run(nativeBin, scriptPath);
            if (timedOut) {
                t.Check(false, $"native run of the generated math sweep did not finish within {NativeOracle.TimeoutMs}ms");
                return t.Finish("MathDifferentialTest");
            }
            if (exitCode != 0) {
                t.Check(false, $"native run of the generated math sweep failed (exit {exitCode}): {stderr}");
                return t.Finish("MathDifferentialTest");
            }

            string[] native = stdout.Split('\n');
            t.CheckEquals(lines.Count + 1, native.Length,
                "native printed exactly one line per case, plus the trailing newline's empty tail");

            for (int i = 0; i < lines.Count && i < native.Length - 1; i++) {
                (string expr, object expectedValue) = lines[i];
                string expected = LoxOps.Stringify(expectedValue);
                string actual = native[i];
                t.CheckEquals(expected, actual, $"{expr} (native line {i})");
            }
        } finally {
            File.Delete(scriptPath);
        }

        return t.Finish("MathDifferentialTest");
    }

    /// <summary>
    /// A math function argument error (e.g. a future function that
    /// rejects some probe outside its domain) must show up as a mismatch
    /// against native's own output for that line, not as an unhandled
    /// exception that stops the whole sweep before it reaches the cases
    /// that matter.
    /// </summary>
    private static object SafeCall(LoxNative fn, object[] args) {
        try {
            return fn.Call(args);
        } catch (Exception ex) {
            return $"<threw {ex.GetType().Name}>";
        }
    }

    private readonly struct Probe {
        public readonly double Value;
        public readonly string SourceExpr;
        public Probe(double value, string sourceExpr) {
            Value = value;
            SourceExpr = sourceExpr;
        }
    }

    private static Probe Literal(double v) {
        string magnitude = NativeOracle.ToFixedDecimalLiteral(Math.Abs(v));
        return new Probe(v, double.IsNegative(v) ? "-" + magnitude : magnitude);
    }

    private static Probe[] BuildProbes(LoxInstance math) {
        double nan = (double)math.Fields["nan"];
        double inf = (double)math.Fields["inf"];
        return new[] {
            Literal(0.0), Literal(-0.0),
            Literal(1.0), Literal(-1.0),
            Literal(0.5), Literal(-0.5),
            Literal(2.0), Literal(-2.0),
            Literal(3.0), Literal(-3.0),
            Literal(10.0), Literal(-10.0),
            Literal(0.1), Literal(1.0 / 3.0),
            Literal(1e10), Literal(1e-10),
            new Probe(nan, "math.nan"),
            new Probe(inf, "math.inf"),
            new Probe(-inf, "-math.inf"),
        };
    }
}
