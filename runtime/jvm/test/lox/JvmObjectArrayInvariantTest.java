package lox;

import static lox.TestSupport.check;

/**
 * Enforces the BINDING INVARIANT documented at LoxOps.getIndex: no Lox
 * value may be a bare Object[]. The captured-local discriminator
 * (ensureCapturedCell, jvm_emitter.cpp) tells a ref-cell from a raw value
 * at run time with `instanceof Object[]`; a bare Object[] handed back as a
 * Lox value would silently misread as a cell the first time it reached a
 * captured local slot, with no verifier error and no exception.
 */
public final class JvmObjectArrayInvariantTest {
    private static void checkNotBareObjectArray(Object value, String description) {
        check(!(value instanceof Object[]), description + " must not be a bare Object[]");
    }

    public static void main(String[] args) {
        // Prove the check itself can fail before trusting it below: an
        // actual Object[] must trip checkNotBareObjectArray.
        Object bareArray = new Object[] {1.0};
        check(bareArray instanceof Object[], "sanity: a bare Object[] is detected as one");

        checkNotBareObjectArray(LoxOps.buildList(new Object[] {1.0, 2.0}), "buildList's result");
        checkNotBareObjectArray(LoxOps.buildMap(new Object[] {"a", 1.0}), "buildMap's result");

        LoxList list = LoxOps.buildList(new Object[] {"x", "y"});
        checkNotBareObjectArray(LoxOps.getIndex(list, 0.0), "getIndex on a list");

        LoxMap map = LoxOps.buildMap(new Object[] {"k", 42.0});
        checkNotBareObjectArray(LoxOps.getIndex(map, "k"), "getIndex on a map");

        // The one legitimate Object[] in this runtime (LoxEnum.payload)
        // must stay wrapped inside its LoxEnum, never handed back bare.
        LoxEnumCtor pair = new LoxEnumCtor(1, 2, "Pair", "Tuple");
        Object built = LoxOps.call(pair, new Object[] {1.0, 2.0});
        checkNotBareObjectArray(built, "an enum constructor's result");
        check(built instanceof LoxEnum, "an enum constructor's result is a LoxEnum, not its raw payload");

        LoxClass box = new LoxClass("Box", null);
        Object instance = LoxOps.call(box, new Object[0]);
        checkNotBareObjectArray(instance, "constructing a class");

        System.exit(TestSupport.finish("JvmObjectArrayInvariantTest"));
    }
}
