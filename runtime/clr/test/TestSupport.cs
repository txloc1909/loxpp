using System;

namespace LoxRuntimeTests;

/// <summary>No test framework in the image (see RT.md) - a tiny shared checker instead. Each suite gets its own instance.</summary>
public sealed class TestSupport {
    private int m_checks;
    private int m_failures;

    public void Check(bool condition, string description) {
        m_checks++;
        if (!condition) {
            m_failures++;
            Console.Error.WriteLine("FAIL: " + description);
        }
    }

    public void CheckEquals(object expected, object actual, string description) {
        bool ok = expected == null ? actual == null : expected.Equals(actual);
        Check(ok, $"{description} (expected <{expected}> but got <{actual}>)");
    }

    public void CheckThrows(Action action, Type expectedExceptionType, string description) {
        try {
            action();
            Check(false, $"{description} (expected {expectedExceptionType.Name} but nothing was thrown)");
        } catch (Exception t) {
            Check(expectedExceptionType.IsInstanceOfType(t),
                $"{description} (expected {expectedExceptionType.Name} but got {t.GetType().Name}: {t.Message})");
        }
    }

    /// <summary>Prints the suite's summary and returns its exit code (0 if every check passed, 1 otherwise).</summary>
    public int Finish(string suiteName) {
        Console.WriteLine($"{suiteName}: {m_checks - m_failures}/{m_checks} checks passed");
        return m_failures == 0 ? 0 : 1;
    }
}
