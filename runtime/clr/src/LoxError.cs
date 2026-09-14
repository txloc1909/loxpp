using System;

namespace Lox;

/// <summary>
/// A Lox++ runtime error. Uncaught, it aborts the program (see vm.cpp runtimeError).
/// When constructed with a Value (for a try/catch scenario), the Value holds an Error
/// instance with message and kind fields per spec/03-types.md.
/// </summary>
public sealed class LoxError : Exception {
    /// <summary>The Error instance being thrown (if this is a caught/catchable error).</summary>
    public object Value { get; }

    /// <summary>Construct from a message only (for uncatchable internal errors).</summary>
    public LoxError(string message) : base(message) {
        Value = null;
    }

    /// <summary>Construct from an Error value (for catchable runtime faults).</summary>
    public LoxError(object value) : base(ExtractMessage(value)) {
        Value = value;
    }

    private static string ExtractMessage(object value) {
        if (value is LoxInstance inst && inst.Fields.TryGetValue("message", out object msg)) {
            return msg as string ?? "Error";
        }
        return "Error";
    }
}
