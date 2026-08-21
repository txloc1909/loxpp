using System;

namespace Lox;

/// <summary>A Lox++ runtime error. Uncaught, it aborts the program (see vm.cpp runtimeError).</summary>
public sealed class LoxError : Exception {
    public LoxError(string message) : base(message) {}
}
