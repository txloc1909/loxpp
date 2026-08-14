package lox;

/**
 * The callable constructor an `enum` declaration produces for each variant.
 * Calling it — even with zero arguments — allocates a fresh {@link LoxEnum};
 * the bare constructor name is not a value on its own (see
 * bytecode-translation-problems.md: nullary variants "do not auto-construct").
 */
public final class LoxEnumCtor implements LoxCallable {
    public final int tag;
    public final int arity;
    public final String ctorName;
    public final String enumName;

    public LoxEnumCtor(int tag, int arity, String ctorName, String enumName) {
        this.tag = tag;
        this.arity = arity;
        this.ctorName = ctorName;
        this.enumName = enumName;
    }

    @Override
    public Object call(Object[] args) {
        if (args.length != arity) {
            throw new LoxError("'" + ctorName + "' expects " + arity
                    + " argument(s) but got " + args.length + ".");
        }
        return new LoxEnum(this, args.clone());
    }
}
