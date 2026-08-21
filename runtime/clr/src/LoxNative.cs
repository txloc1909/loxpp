namespace Lox;

/// <summary>Wraps a host-provided (C#) function as a callable Lox++ value.</summary>
public sealed class LoxNative : ILoxCallable {
    /// <summary>A native function body.</summary>
    public delegate object Fn(object[] args);

    public readonly string Name;
    public readonly int Arity; // -1 = variadic, matching ObjNative::arity
    private readonly Fn m_fn;

    public LoxNative(string name, int arity, Fn fn) {
        Name = name;
        Arity = arity;
        m_fn = fn;
    }

    public object Call(object[] args) {
        if (Arity != -1 && args.Length != Arity) {
            throw new LoxError($"Expected {Arity} arguments but got {args.Length}.");
        }
        return m_fn(args);
    }
}
