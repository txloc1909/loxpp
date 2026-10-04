package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

/**
 * Direct tests for the coroutine runtime (mission #523 node #529). Generated
 * code is not involved: each coroutine's function is a hand-written
 * {@link LoxClosure} that calls {@link LoxOps#yield(Object)} itself.
 */
public final class CoroutineTest {
    private static final Object[] NO_ARGS = new Object[0];

    public static void main(String[] args) {
        checkYieldOutsideCoroutine();
        checkResumeAndStatus();
        checkDeadAndRunningFaults();
        checkYieldAcrossNative();
        checkIteration();
        checkValuePresentation();

        System.exit(TestSupport.finish("CoroutineTest"));
    }

    private static String kindOf(LoxError e) {
        // A catchable fault carries a makeError() Error instance; a raw one
        // would rethrow through getValue() and this test would fail loudly.
        return (String)((LoxInstance)e.getValue()).fields.get("kind");
    }

    private static void checkYieldOutsideCoroutine() {
        LoxError err = null;
        try {
            LoxOps.yield(1.0);
        } catch (LoxError e) {
            err = e;
        }
        check(err != null && "YieldOutsideCoroutineError".equals(kindOf(err)),
              "yield outside a coroutine is a YieldOutsideCoroutineError");
    }

    private static void checkResumeAndStatus() {
        LoxCoroutine[] self = new LoxCoroutine[1];
        LoxClosure body = new LoxClosure("gen", 0, new Object[0][]) {
            @Override
            protected Object invoke(Object s, Object[] a) {
                checkEquals("running", self[0].statusName(),
                            "a coroutine is running while its function runs");
                Object x = LoxOps.yield(1.0);
                Object y = LoxOps.yield(2.0);
                return (Double)x + (Double)y;
            }
        };
        LoxCoroutine co = new LoxCoroutine(body);
        self[0] = co;

        checkEquals("suspended", co.statusName(), "a fresh coroutine is suspended");
        checkEquals(1.0, co.resume(NO_ARGS), "first resume returns the first yield");
        checkEquals("suspended", co.statusName(), "a yielded coroutine is suspended");
        checkEquals(2.0, co.resume(new Object[] {10.0}),
                    "resume's value fills the pending yield");
        checkEquals(42.0, co.resume(new Object[] {32.0}),
                    "a return is delivered to the resumer");
        checkEquals("dead", co.statusName(), "a returned coroutine is dead");
    }

    private static void checkDeadAndRunningFaults() {
        LoxClosure returns = new LoxClosure("done", 0, new Object[0][]) {
            @Override
            protected Object invoke(Object s, Object[] a) {
                return "done";
            }
        };
        LoxCoroutine dead = new LoxCoroutine(returns);
        checkEquals("done", dead.resume(NO_ARGS), "a returning coroutine yields");
        checkEquals("dead", dead.statusName(), "and is dead");
        LoxError deadErr = null;
        try {
            dead.resume(NO_ARGS);
        } catch (LoxError e) {
            deadErr = e;
        }
        check(deadErr != null &&
                  "DeadCoroutineError".equals(kindOf(deadErr)),
              "resuming a dead coroutine is a DeadCoroutineError");

        LoxCoroutine[] self = new LoxCoroutine[1];
        LoxClosure recurse = new LoxClosure("self", 0, new Object[0][]) {
            @Override
            protected Object invoke(Object s, Object[] a) {
                self[0].resume(NO_ARGS);
                return null;
            }
        };
        LoxCoroutine running = new LoxCoroutine(recurse);
        self[0] = running;
        LoxError runErr = null;
        try {
            running.resume(NO_ARGS);
        } catch (LoxError e) {
            runErr = e;
        }
        check(runErr != null &&
                  "RunningCoroutineError".equals(kindOf(runErr)),
              "resuming a running coroutine is a RunningCoroutineError");
    }

    private static void checkYieldAcrossNative() {
        LoxClosure body = new LoxClosure("cb", 0, new Object[0][]) {
            @Override
            protected Object invoke(Object s, Object[] a) {
                LoxOps.enterCallback();
                try {
                    return LoxOps.yield(1.0);
                } finally {
                    LoxOps.exitCallback();
                }
            }
        };
        LoxCoroutine co = new LoxCoroutine(body);
        LoxError err = null;
        try {
            co.resume(NO_ARGS);
        } catch (LoxError e) {
            err = e;
        }
        check(err != null && "YieldAcrossNativeError".equals(kindOf(err)),
              "a yield under a native callback is a YieldAcrossNativeError");
    }

    private static void checkIteration() {
        LoxClosure gen = new LoxClosure("gen", 0, new Object[0][]) {
            @Override
            protected Object invoke(Object s, Object[] a) {
                LoxOps.yield(7.0);
                LoxOps.yield(8.0);
                return null;
            }
        };
        LoxIterator it = new LoxIterator(new LoxCoroutine(gen));
        check(it.hasNext(), "for-in sees the first yielded element");
        checkEquals(7.0, it.next(), "for-in binds the first yielded element");
        check(it.hasNext(), "for-in sees the second yielded element");
        checkEquals(8.0, it.next(), "for-in binds the second yielded element");
        check(!it.hasNext(), "for-in ends when the coroutine returns");

        LoxClosure empty = new LoxClosure("empty", 0, new Object[0][]) {
            @Override
            protected Object invoke(Object s, Object[] a) {
                return null;
            }
        };
        check(!new LoxIterator(new LoxCoroutine(empty)).hasNext(),
              "for-in over a coroutine that returns at once exits");
    }

    private static void checkValuePresentation() {
        LoxCoroutine co = new LoxCoroutine(
            new LoxClosure("f", 0, new Object[0][]) {
                @Override
                protected Object invoke(Object s, Object[] a) {
                    return null;
                }
            });
        checkEquals("<coroutine>", LoxOps.str(co), "str(coroutine)");
        LoxGlobals globals = LoxRuntime.init();
        Object typeName =
            ((LoxCallable)globals.get("type")).call(new Object[] {co});
        checkEquals("Coroutine", typeName, "type(coroutine)");
    }
}
