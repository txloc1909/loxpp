package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

/** LoxOps.call (the CALL opcode's runtime dispatch) and LoxRuntime.current() — node N6. */
public final class CallDispatchTest {
    public static void main(String[] args) {
        LoxClosure add = new LoxClosure("add", 2, new Object[0][]) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                return LoxOps.add(a[0], a[1]);
            }
        };
        checkEquals(3.0, LoxOps.call(add, new Object[] {1.0, 2.0}), "call() dispatches to a closure");
        checkThrows(() -> LoxOps.call(add, new Object[] {1.0}), LoxError.class,
                "call() still enforces the callee's own arity check");

        LoxNative clock = new LoxNative("clock", 0, a -> 42.0);
        checkEquals(42.0, LoxOps.call(clock, new Object[0]), "call() dispatches to a native");

        LoxClass box = new LoxClass("Box", null);
        Object instance = LoxOps.call(box, new Object[0]);
        check(instance instanceof LoxInstance, "call() dispatches to a class (construction)");

        LoxEnumCtor ok = new LoxEnumCtor(1, 1, "Ok", "Result");
        Object enumVal = LoxOps.call(ok, new Object[] {5.0});
        check(enumVal instanceof LoxEnum, "call() dispatches to an enum constructor");

        LoxBoundMethod bound = new LoxBoundMethod(instance, add);
        checkEquals(3.0, LoxOps.call(bound, new Object[] {1.0, 2.0}), "call() dispatches to a bound method");

        checkThrows(() -> LoxOps.call(1.0, new Object[0]), LoxError.class,
                "call() rejects a value that is not callable");
        checkThrows(() -> LoxOps.call(null, new Object[0]), LoxError.class,
                "call() rejects nil the same way as any other non-callable value");

        LoxGlobals g = LoxRuntime.init();
        check(LoxRuntime.current() == g, "current() returns the instance init() just built");

        System.exit(TestSupport.finish("CallDispatchTest"));
    }
}
