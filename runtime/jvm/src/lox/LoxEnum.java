package lox;

/**
 * Equality is identity, not structural (Object's default is left alone on
 * purpose): two separate constructions of the same variant with the same
 * payload are never equal — `Green() == Green()` is `false` — per
 * bytecode-translation-problems.md's "construct by identity" finding.
 */
public final class LoxEnum {
    public final LoxEnumCtor ctor;
    public final Object[] payload;

    public LoxEnum(LoxEnumCtor ctor, Object[] payload) {
        this.ctor = ctor;
        this.payload = payload;
    }
}
