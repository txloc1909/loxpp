using System;
using System.Linq;
using System.Reflection;

namespace LoxRuntimeTests;

/// <summary>
/// No test framework in the build container - suites are discovered by
/// name instead of hardcoded, so a new `*Test.cs` file with a public
/// `static int Run()` is picked up on its own, matching
/// tools/test_lox_rt.sh's own `*Test.java` discovery for the JVM runtime.
/// </summary>
public static class Program {
    public static int Main() {
        Type[] suiteTypes = Assembly.GetExecutingAssembly().GetTypes()
            .Where(t => t.IsClass && t.Name.EndsWith("Test", StringComparison.Ordinal))
            .OrderBy(t => t.Name, StringComparer.Ordinal)
            .ToArray();

        int failures = 0;
        foreach (Type suiteType in suiteTypes) {
            MethodInfo method = suiteType.GetMethod("Run", BindingFlags.Public | BindingFlags.Static);
            if (method == null) {
                Console.Error.WriteLine($"Program: {suiteType.Name} has no public static Run() method.");
                failures++;
                continue;
            }
            Console.WriteLine($"== {suiteType.Name} ==");
            var result = (int)method.Invoke(null, null);
            if (result != 0) {
                failures++;
            }
        }

        Console.WriteLine();
        if (failures != 0) {
            Console.Error.WriteLine($"{failures} lox-rt test suite(s) failed.");
            return 1;
        }
        Console.WriteLine("All lox-rt test suites passed.");
        return 0;
    }
}
