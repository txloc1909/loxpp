using System;
using System.Reflection;
using System.Runtime.ExceptionServices;
using System.Threading;

namespace Lox;

/// <summary>
/// Runs an emitted Lox++ program's entry point on a thread whose stack size
/// is chosen at thread-creation time, not inherited from the OS process.
///
/// CoreCLR gives no command-line switch to grow the process's own main
/// thread (unlike the JVM's <c>-Xss</c>): a thread's stack size is fixed
/// when that thread is created, and the main thread already exists by the
/// time managed code runs. <c>LoxOps.Stringify</c> recurses once per level
/// of list/map nesting with no depth guard of its own, unbounded by
/// <see cref="LoxClosure"/>'s frame ceiling — measured to exhaust the
/// default 8 MiB Linux thread stack partway through formatting the
/// resulting exception's own message, which CoreCLR then reports as a
/// fatal process abort rather than the ordinary unhandled-exception path
/// below (notes/translation-probes/clr-only/39_deep_nested_stringify.lox;
/// full measurement in notes/clr-backend-completion.md). Running the same
/// generated code on a thread with far more headroom does change whether
/// an unguarded-recursion case like that one succeeds: it fails on an
/// 8 MiB thread and prints the correct output on the 256 MiB one this
/// host provides. What stays the same at any thread size is Lox-level
/// call recursion: <see cref="LoxClosure"/>'s own frame ceiling is
/// unchanged, and matches native's, so a program whose call depth alone
/// overflows it gets the identical <c>Stack overflow.</c> error either
/// way.
///
/// <c>tools/clr_run.sh</c> runs this assembly in place of an emitted
/// program's own assembly, for every CLR invocation, not only the
/// bootstrap interpreter's — the indirection costs one reflection call and
/// one thread per run, and no emitted program depends on which thread its
/// own body executes on.
/// </summary>
public static class LoxHost {
    /// <summary>
    /// Usage: LoxHost &lt;stack-bytes&gt; &lt;program-assembly-path&gt; [arg...].
    /// Loads the target assembly, invokes its own entry point with [arg...]
    /// on a new thread, and returns that entry point's own exit behavior:
    /// its int return value if it has one, 0 if it returns void and does
    /// not throw, or an unhandled-exception process termination if it
    /// throws — the same three outcomes running it directly would give.
    /// </summary>
    public static int Main(string[] hostArgs) {
        if (hostArgs.Length < 2) {
            Console.Error.WriteLine(
                "usage: LoxHost <stack-bytes> <program-assembly-path> [arg...]");
            return 2;
        }

        if (!int.TryParse(hostArgs[0], out int stackBytes) || stackBytes < 0) {
            Console.Error.WriteLine(
                $"LoxHost: invalid stack-bytes argument: '{hostArgs[0]}' is not a " +
                "non-negative integer");
            return 2;
        }
        string assemblyPath = hostArgs[1];
        string[] programArgs = new string[hostArgs.Length - 2];
        Array.Copy(hostArgs, 2, programArgs, 0, programArgs.Length);

        Assembly program = Assembly.LoadFrom(assemblyPath);
        MethodInfo entry = program.EntryPoint;
        if (entry == null) {
            Console.Error.WriteLine($"LoxHost: {assemblyPath} declares no entry point");
            return 2;
        }
        object[] entryArgs = entry.GetParameters().Length == 0
            ? Array.Empty<object>()
            : new object[] { programArgs };

        int exitCode = 0;
        var thread = new Thread(() => {
            object result;
            try {
                result = entry.Invoke(null, entryArgs);
            } catch (TargetInvocationException wrapped) when (wrapped.InnerException != null) {
                // Reflection wraps every exception the invoked method itself
                // throws. Re-throwing the original, with its own original
                // stack trace preserved, keeps this indirection invisible to
                // anything that inspects the uncaught exception's own type or
                // text (tools/check_clr_probes.sh's error probes grep stderr
                // for "Lox.LoxError").
                ExceptionDispatchInfo.Capture(wrapped.InnerException).Throw();
                throw; // unreachable; satisfies the compiler's definite-assignment check.
            }
            exitCode = result is int i ? i : 0;
        }, stackBytes);
        thread.Start();
        thread.Join();
        return exitCode;
    }
}
