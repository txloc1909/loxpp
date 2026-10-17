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

    /**
     * True when every field is a message (spec/03-types.md, Messages). Fixed
     * here, at construction: no construct assigns a field of an enum value,
     * so a send reads this bit and never walks the fields. The JVM heap is
     * shared, so the bit, not the heap, is what refuses a mutable value.
     */
    final boolean message;

    public LoxEnum(LoxEnumCtor ctor, Object[] payload) {
        this.ctor = ctor;
        this.payload = payload;
        boolean all = true;
        for (Object field : payload) {
            if (!LoxTask.isMessage(field)) {
                all = false;
                break;
            }
        }
        this.message = all;
    }
}
