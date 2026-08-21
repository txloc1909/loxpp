using System.Collections.Generic;

namespace Lox;

/// <summary>
/// An instance's fields are open-ended (spring into existence on first
/// assignment - spec/03-types.md), so a plain map is the field table; no
/// per-class field layout exists. Identity equality (Lox default for this
/// type) comes for free from <see cref="object"/>, so Equals/GetHashCode
/// are left alone.
/// </summary>
public sealed class LoxInstance {
    public readonly LoxClass Klass;
    public readonly Dictionary<string, object> Fields = new();

    public LoxInstance(LoxClass klass) {
        Klass = klass;
    }
}
