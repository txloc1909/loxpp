package lox;

/** Wraps a host-provided (Java) function as a callable Lox++ value. */
public final class LoxNative implements LoxCallable {
    /** A native function body. Named Fn, not java.util.function.Function, so call sites read as Lox natives. */
    public interface Fn {
        Object apply(Object[] args);
    }

    public final String name;
    public final int arity; // -1 = variadic, matching ObjNative::arity
    private final Fn fn;

    public LoxNative(String name, int arity, Fn fn) {
        this.name = name;
        this.arity = arity;
        this.fn = fn;
    }

    @Override
    public Object call(Object[] args) {
        if (arity != -1 && args.length != arity) {
            throw new LoxError(
                    "Expected " + arity + " arguments but got " + args.length + ".");
        }
        return fn.apply(args);
    }
}
