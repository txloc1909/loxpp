using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Lox;

namespace LoxRuntimeTests;

/// <summary>
/// Differential parity for the `globals` + `os_api` surface, the same
/// shape as MathDifferentialTest and StringifyDifferentialTest: native
/// build/loxpp is the oracle, run through <see cref="NativeOracle"/>, and
/// this runtime's own computation of the same call is compared against
/// its real printed line.
///
/// <para>The fixture tree built in <see cref="BuildFixtures"/> is the
/// point: <c>exists</c>/<c>is_dir</c>/<c>is_file</c>/<c>stat</c> cannot be
/// swept meaningfully with only ordinary files - the defect they are
/// prone to is in the FILE TYPE, so the fixtures include a dangling
/// symbolic link, a symbolic link to a directory, a named pipe, and a
/// character device, alongside a plain file and an empty directory.</para>
///
/// <para><c>stat</c>'s result is compared key by key
/// (<see cref="AddPathChecks"/>), never by stringifying the whole map:
/// key order is the known, accepted map-order difference between this
/// runtime and native, not a defect this sweep should report.</para>
///
/// <para><c>open()</c> is swept separately, in
/// <see cref="AddOpenChecks"/>: a failing open is a Lox++ runtime error
/// with no catch construct in the language, so unlike every other check
/// here it cannot share one batched script with the rest - one bad case
/// would abort the whole native run before printing any later line. Each
/// open case therefore runs as its own single-purpose script, and only
/// the success/failure outcome is compared (differential scope is stdout
/// only, and a failed open's message never reaches it).</para>
/// </summary>
public static class StdlibDifferentialTest {
    public static int Run() {
        var t = new TestSupport();

        string nativeBin = NativeOracle.RequireFitBinary(t);
        if (nativeBin == null) {
            return t.Finish("StdlibDifferentialTest");
        }

        LoxGlobals globals = LoxRuntime.Init();
        string fixtureRoot = Path.Combine(Path.GetTempPath(), $"lox-rt-stdlib-diff-{Guid.NewGuid():N}");
        try {
            Fixtures fx = BuildFixtures(fixtureRoot);

            var lines = new List<(string Script, object Expected)>();
            AddPathChecks(t, lines, globals, fx.RegularFile, expectExists: true);
            AddPathChecks(t, lines, globals, fx.EmptyDir, expectExists: true);
            AddPathChecks(t, lines, globals, fx.DanglingSymlink, expectExists: false);
            AddPathChecks(t, lines, globals, fx.DirSymlink, expectExists: true);
            AddPathChecks(t, lines, globals, fx.Fifo, expectExists: true);
            AddPathChecks(t, lines, globals, fx.CharDevice, expectExists: true);
            AddEnvChecks(t, lines, globals);
            AddLenAndStrChecks(lines, globals);

            RunBatch(t, nativeBin, lines);
            AddOpenChecks(t, nativeBin, globals, fixtureRoot);
        } finally {
            try {
                Directory.Delete(fixtureRoot, recursive: true);
            } catch (IOException) {
                // best effort - a stray open handle on a fixture must not
                // fail the whole suite over cleanup
            }
        }

        return t.Finish("StdlibDifferentialTest");
    }

    private readonly struct Fixtures {
        public readonly string RegularFile;
        public readonly string EmptyDir;
        public readonly string DanglingSymlink;
        public readonly string DirSymlink;
        public readonly string Fifo;
        public readonly string CharDevice;

        public Fixtures(string regularFile, string emptyDir, string danglingSymlink, string dirSymlink, string fifo, string charDevice) {
            RegularFile = regularFile;
            EmptyDir = emptyDir;
            DanglingSymlink = danglingSymlink;
            DirSymlink = dirSymlink;
            Fifo = fifo;
            CharDevice = charDevice;
        }
    }

    [DllImport("libc", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern int mkfifo(string pathname, uint mode);

    private static Fixtures BuildFixtures(string root) {
        Directory.CreateDirectory(root);

        string regularFile = Path.Combine(root, "regular.txt");
        File.WriteAllBytes(regularFile, LoxRuntime.Charset.GetBytes("hello loxpp"));

        string emptyDir = Path.Combine(root, "emptydir");
        Directory.CreateDirectory(emptyDir);

        string missingTarget = Path.Combine(root, "no-such-target");
        string danglingSymlink = Path.Combine(root, "dangling");
        File.CreateSymbolicLink(danglingSymlink, missingTarget);

        string dirSymlink = Path.Combine(root, "dirlink");
        Directory.CreateSymbolicLink(dirSymlink, emptyDir);

        string fifo = Path.Combine(root, "fifo");
        // 0666 (rw-rw-rw-, subject to umask) - matches mkfifo(1)'s own default.
        if (mkfifo(fifo, 0x1B6) != 0) {
            throw new IOException($"mkfifo({fifo}) failed (errno {Marshal.GetLastPInvokeError()})");
        }

        return new Fixtures(regularFile, emptyDir, danglingSymlink, dirSymlink, fifo, "/dev/null");
    }

    private static object Call(LoxGlobals g, string name, params object[] args) {
        return ((ILoxCallable)g.Get(name)).Call(args);
    }

    private static string LoxStringLiteral(string raw) {
        return "\"" + raw.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
    }

    /// <summary>
    /// <paramref name="expectExists"/> is a fact about the fixture this
    /// test just built on disk, not a guess derived from this runtime's
    /// own answer - it decides whether the generated script indexes into
    /// `stat`'s result at all, and native errors on indexing nil with no
    /// way to catch it, so getting this from the wrong source could abort
    /// the whole batch on exactly the case this sweep exists to catch.
    /// </summary>
    private static void AddPathChecks(TestSupport t, List<(string, object)> lines, LoxGlobals globals, string path, bool expectExists) {
        string lit = LoxStringLiteral(path);
        lines.Add(($"exists({lit})", Call(globals, "exists", path)));
        lines.Add(($"is_dir({lit})", Call(globals, "is_dir", path)));
        lines.Add(($"is_file({lit})", Call(globals, "is_file", path)));
        object statResult = Call(globals, "stat", path);
        lines.Add(($"stat({lit}) == nil", statResult == null));

        if (!expectExists) {
            return;
        }
        if (statResult is not LoxMap map) {
            t.Check(false, $"stat({lit}) was expected to return a Map for a fixture built to exist, got {statResult ?? "nil"}");
            return;
        }
        lines.Add(($"stat({lit})[\"is_dir\"]", map.Get("is_dir")));
        lines.Add(($"stat({lit})[\"is_file\"]", map.Get("is_file")));
        lines.Add(($"\"size\" in stat({lit})", map.Has("size")));
    }

    private static void AddEnvChecks(TestSupport t, List<(string, object)> lines, LoxGlobals globals) {
        const string unsetName = "LOXPP_STDLIB_DIFF_NO_SUCH_VAR_XYZ";
        lines.Add(($"env(\"{unsetName}\") == nil", Call(globals, "env", unsetName) == null));

        // The empty-valued fixture must come from the process's real
        // environment block (tools/test_lox_rt_clr.sh exports it), never
        // from Environment.SetEnvironmentVariable: .NET documents that
        // call as deleting the variable, so a fixture built that way
        // would observe nil instead of "" and prove nothing about a
        // variable that is genuinely defined empty.
        // RuntimeStdlibTest.CheckEnvOfEmptyValueIsNotNil pins the same
        // fact with a fixed expectation.
        const string emptyName = "LOXPP_EMPTY_PROBE";
        if (Environment.GetEnvironmentVariable(emptyName) == "") {
            lines.Add(($"env(\"{emptyName}\")", Call(globals, "env", emptyName)));
        } else {
            t.Check(false, $"{emptyName} must be exported empty before this sweep runs - see tools/test_lox_rt_clr.sh");
        }

        if (Environment.GetEnvironmentVariable("PATH") != null) {
            lines.Add(("env(\"PATH\") == nil", Call(globals, "env", "PATH") == null));
        }
    }

    /// <summary>
    /// A map's own printed form is excluded here on purpose - its key
    /// order is the known map-order difference, not something this test
    /// should pin - except for a single-key map, where no order question
    /// exists at all.
    /// </summary>
    private static void AddLenAndStrChecks(List<(string, object)> lines, LoxGlobals globals) {
        lines.Add(("str(42)", Call(globals, "str", 42.0)));
        lines.Add(("str(true)", Call(globals, "str", true)));
        lines.Add(("str(false)", Call(globals, "str", false)));
        // A bare `null` here would bind to the params array itself (C#'s
        // params-array/null-literal ambiguity), not to one nil argument.
        lines.Add(("str(nil)", Call(globals, "str", new object[] { null })));
        lines.Add(("str(\"hello\")", Call(globals, "str", "hello")));
        lines.Add(("len(\"hello\")", Call(globals, "len", "hello")));
        lines.Add(("len(\"\")", Call(globals, "len", "")));

        var list = new LoxList();
        list.Elements.Add(1.0);
        list.Elements.Add(2.0);
        list.Elements.Add(3.0);
        lines.Add(("str([1, 2, 3])", Call(globals, "str", list)));
        lines.Add(("len([1, 2, 3])", Call(globals, "len", list)));
        lines.Add(("len([])", Call(globals, "len", new LoxList())));

        var singleKeyMap = new LoxMap();
        singleKeyMap.Put("a", 1.0);
        lines.Add(("str({\"a\": 1})", Call(globals, "str", singleKeyMap)));
        lines.Add(("len({\"a\": 1})", Call(globals, "len", singleKeyMap)));
        lines.Add(("len({})", Call(globals, "len", new LoxMap())));
    }

    private static void RunBatch(TestSupport t, string nativeBin, List<(string Script, object Expected)> lines) {
        string scriptPath = Path.Combine(Path.GetTempPath(), $"lox-rt-stdlib-diff-{Guid.NewGuid():N}.lox");
        try {
            var sb = new StringBuilder();
            foreach ((string expr, _) in lines) {
                sb.Append("print ").Append(expr).Append(";\n");
            }
            File.WriteAllText(scriptPath, sb.ToString(), Encoding.ASCII);

            (int exitCode, string stdout, string stderr, bool timedOut) = NativeOracle.Run(nativeBin, scriptPath);
            if (timedOut) {
                t.Check(false, $"native run of the generated stdlib sweep did not finish within {NativeOracle.TimeoutMs}ms");
                return;
            }
            if (exitCode != 0) {
                t.Check(false, $"native run of the generated stdlib sweep failed (exit {exitCode}): {stderr}");
                return;
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
    }

    private static void AddOpenChecks(TestSupport t, string nativeBin, LoxGlobals globals, string fixtureRoot) {
        foreach (string mode in new[] { "r", "w", "a", "r+" }) {
            CheckOpenOutcome(t, nativeBin, globals, fixtureRoot, mode, pathExists: true);
            CheckOpenOutcome(t, nativeBin, globals, fixtureRoot, mode, pathExists: false);
        }
    }

    private static void CheckOpenOutcome(TestSupport t, string nativeBin, LoxGlobals globals, string fixtureRoot, string mode, bool pathExists) {
        string path = Path.Combine(fixtureRoot, $"open-{mode.Replace('+', 'p')}-{(pathExists ? "present" : "missing")}.txt");
        if (pathExists) {
            File.WriteAllBytes(path, LoxRuntime.Charset.GetBytes("x"));
        }

        string script = $"var f = open({LoxStringLiteral(path)}, \"{mode}\");\nprint \"opened\";\n";
        string scriptPath = Path.Combine(Path.GetTempPath(), $"lox-rt-stdlib-diff-open-{Guid.NewGuid():N}.lox");
        bool nativeOpened;
        try {
            File.WriteAllText(scriptPath, script, Encoding.ASCII);
            (int exitCode, string stdout, string _, bool timedOut) = NativeOracle.Run(nativeBin, scriptPath);
            if (timedOut) {
                t.Check(false, $"native open(path, \"{mode}\") ({(pathExists ? "present" : "missing")}) did not finish within {NativeOracle.TimeoutMs}ms");
                return;
            }
            nativeOpened = exitCode == 0 && stdout == "opened\n";
        } finally {
            File.Delete(scriptPath);
        }

        bool thisOpened;
        try {
            var file = (LoxFile)Call(globals, "open", path, mode);
            file.Close();
            thisOpened = true;
        } catch (LoxError) {
            thisOpened = false;
        }

        t.CheckEquals(nativeOpened, thisOpened,
            $"open(path, \"{mode}\") on a {(pathExists ? "present" : "missing")} path: native {(nativeOpened ? "succeeds" : "fails")}");
    }
}
