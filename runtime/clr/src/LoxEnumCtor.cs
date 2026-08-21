namespace Lox;

/// <summary>
/// The callable constructor an <c>enum</c> declaration produces for each
/// variant. Calling it - even with zero arguments - allocates a fresh
/// <see cref="LoxEnum"/>; the bare constructor name is not a value on its
/// own (see bytecode-translation-problems.md: nullary variants "do not
/// auto-construct").
/// </summary>
public sealed class LoxEnumCtor : ILoxCallable {
    public readonly int Tag;
    public readonly int Arity;
    public readonly string CtorName;
    public readonly string EnumName;

    public LoxEnumCtor(int tag, int arity, string ctorName, string enumName) {
        Tag = tag;
        Arity = arity;
        CtorName = ctorName;
        EnumName = enumName;
    }

    public object Call(object[] args) {
        if (args.Length != Arity) {
            throw new LoxError($"'{CtorName}' expects {Arity} argument(s) but got {args.Length}.");
        }
        return new LoxEnum(this, (object[])args.Clone());
    }
}
