package lox;

/**
 * Base class for every generated function/method body. Holds the upvalue
 * cells captured at CLOSURE time — one {@code Object[1]} ref-cell per
 * upvalue, matching the fresh-cell-per-declaration model in
 * bytecode-translation-problems.md (P4): CLOSE_UPVALUE ends a cell's live
 * range, it does not merely no-op.
 *
 * Lox slot 0 is "the callee itself — or, in a method, the receiver `this`"
 * (same doc, root-cause section). {@link #call} binds slot 0 to this closure,
 * matching a plain function reading its own identity; {@link #callAsSelf}
 * lets a bound-method/INVOKE/SUPER_INVOKE call site bind slot 0 to the
 * receiver instead. `arity` excludes slot 0, matching ObjFunction::arity.
 */
public abstract class LoxClosure implements LoxCallable {
    public final String name; // null for the top-level script, per <script>
    public final int arity;
    public final Object[][] upvalues;

    protected LoxClosure(String name, int arity, Object[][] upvalues) {
        this.name = name;
        this.arity = arity;
        this.upvalues = upvalues;
    }

    @Override
    public final Object call(Object[] args) {
        return callAsSelf(this, args);
    }

    public final Object callAsSelf(Object self, Object[] args) {
        if (args.length != arity) {
            throw new LoxError(
                    "Expected " + arity + " arguments but got " + args.length + ".");
        }
        return invoke(self, args);
    }

    protected abstract Object invoke(Object self, Object[] args);
}
