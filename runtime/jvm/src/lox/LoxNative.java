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

    /**
     * Non-null only for a Map/File method value obtained through plain
     * property access (see {@link LoxMap#getMethod} / {@link
     * LoxFile#getMethod}) — the object the method is bound to, mirroring
     * src/exec_objects.h's {@code ObjBoundNative::receiver}. Every other
     * native, including a math field such as {@code math.abs}, which
     * src/stdlib/math_module.cpp stores as a plain unbound native, leaves
     * this null. LoxRuntime's reflection {@code type()} is the only reader:
     * it is what tells a bound Map/File method (type "BoundMethod") apart
     * from an ordinary native (type "Function") when both are instances of
     * this same class.
     */
    public final Object receiver;

    public LoxNative(String name, int arity, Fn fn) {
        this(name, arity, fn, null);
    }

    public LoxNative(String name, int arity, Fn fn, Object receiver) {
        this.name = name;
        this.arity = arity;
        this.fn = fn;
        this.receiver = receiver;
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
