namespace Lox;

/// <summary>
/// Captures a callable and its arguments for deferred execution.
/// Represents a single `defer` statement from Lox++.
/// </summary>
public class DeferredCall
{
    public readonly object? Callable;
    public readonly object?[] Args;

    public DeferredCall(object? callable, object?[] args)
    {
        Callable = callable;
        Args = args;
    }
}
