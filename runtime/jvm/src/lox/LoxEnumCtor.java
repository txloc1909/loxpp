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
            // Native's own text (src/vm.cpp's CALL, isEnumCtor branch) is
            // this exact literal — it does not name the constructor or
            // report either count.
            throw LoxOps.makeError("ConstructorArityError",
                    "Constructor called with wrong arity.");
        }
        return new LoxEnum(this, args.clone());
    }
}
