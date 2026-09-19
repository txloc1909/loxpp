using System;

namespace Lox;

/// <summary>
/// A Lox++ runtime error. Uncaught, it aborts the program (see vm.cpp runtimeError).
/// When constructed with a Value (for a try/catch scenario), the Value holds an Error
/// instance with message and kind fields per spec/03-types.md.
///
/// <see cref="Catchable"/> mirrors src/vm.cpp's own split between a fault
/// raised through <c>tryCatchableError</c>/<c>raiseThrowableError</c> (goes
/// through <c>handleThrow</c>, so a live Lox++ <c>try</c> can catch it) and
/// one raised through <c>RAISE_ERROR</c>/plain <c>runtimeError</c> (bypasses
/// <c>handleThrow</c> entirely, so no handler ever sees it, however many
/// live <c>try</c> blocks surround the fault site). The emitted CLR
/// <c>catch [LoxRuntime]Lox.LoxError</c> block (src/backend/clr_emitter.cpp)
/// reads this flag before running any handler body and rethrows when it is
/// false, so an uncatchable fault propagates past every live handler here
/// too, instead of being delivered with <see cref="Value"/> silently null.
/// </summary>
public sealed class LoxError : Exception {
    /// <summary>The Error instance being thrown (if this is a caught/catchable error).</summary>
    private object _value;
    public object Value { get { return _value; } }

    private bool _catchable;
    public bool Catchable { get { return _catchable; } }

    /// <summary>Construct from a message only (for uncatchable internal errors).</summary>
    public LoxError(string message) : base(message) {
        _value = null;
        _catchable = false;
    }

    /// <summary>
    /// Construct from a value (for a catchable runtime fault, built via
    /// <see cref="LoxRuntime.MakeError"/>, or for the exact value a Lox++
    /// <c>throw</c> statement passed — which reaches a live handler
    /// regardless of its own type, including <c>nil</c>).
    /// </summary>
    public LoxError(object value) : base(ExtractMessage(value)) {
        _value = value;
        _catchable = true;
    }

    private static string ExtractMessage(object value) {
        if (value is LoxInstance inst && inst.Fields.TryGetValue("message", out object msg)) {
            return msg as string ?? "Error";
        }
        return "Error";
    }
}
