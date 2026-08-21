namespace Lox;

/// <summary>
/// Base class for every generated function/method body. Holds the upvalue
/// cells captured at CLOSURE time - one one-element <c>object[]</c> ref-cell
/// per upvalue, matching the fresh-cell-per-declaration model in
/// bytecode-translation-problems.md (P4): CLOSE_UPVALUE ends a cell's live
/// range, it does not merely no-op.
///
/// Lox slot 0 is "the callee itself - or, in a method, the receiver `this`"
/// (same doc, root-cause section). <see cref="Call"/> binds slot 0 to this
/// closure, matching a plain function reading its own identity;
/// <see cref="CallAsSelf"/> lets a bound-method/INVOKE/SUPER_INVOKE call
/// site bind slot 0 to the receiver instead. <c>Arity</c> excludes slot 0,
/// matching ObjFunction::arity.
/// </summary>
public abstract class LoxClosure : ILoxCallable {
    public readonly string Name; // null for the top-level script, per <script>
    public readonly int Arity;
    public readonly object[][] Upvalues;

    protected LoxClosure(string name, int arity, object[][] upvalues) {
        Name = name;
        Arity = arity;
        Upvalues = upvalues;
    }

    public object Call(object[] args) => CallAsSelf(this, args);

    public object CallAsSelf(object self, object[] args) {
        if (args.Length != Arity) {
            throw new LoxError($"Expected {Arity} arguments but got {args.Length}.");
        }
        return Invoke(self, args);
    }

    protected abstract object Invoke(object self, object[] args);
}
