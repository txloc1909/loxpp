package lox;

/** No JUnit in the image (see RT.md) — a tiny shared checker instead. Each suite's main() ends with System.exit(finish(...)). */
public final class TestSupport {
    private static int checks = 0;
    private static int failures = 0;

    private TestSupport() {}

    public static void check(boolean condition, String description) {
        checks++;
        if (!condition) {
            failures++;
            System.err.println("FAIL: " + description);
        }
    }

    public static void checkEquals(Object expected, Object actual, String description) {
        boolean ok = (expected == null) ? (actual == null) : expected.equals(actual);
        check(ok, description + " (expected <" + expected + "> but got <" + actual + ">)");
    }

    public static void checkThrows(Runnable action, Class<? extends Throwable> expected, String description) {
        try {
            action.run();
            check(false, description + " (expected " + expected.getSimpleName() + " but nothing was thrown)");
        } catch (Throwable t) {
            check(expected.isInstance(t),
                    description + " (expected " + expected.getSimpleName() + " but got "
                            + t.getClass().getSimpleName() + ": " + t.getMessage() + ")");
        }
    }

    /**
     * Asserts that the action raises a fatal Lox++ fault: an internally raised
     * one, which no Lox++ `try` can catch ({@link LoxError#isCatchable()} is
     * false). A catchable fault is a Lox++ value, such as the NetworkError of
     * a refused connection, so a `try` in the program would catch it and it
     * fails here. Use this where the kind of fault, not only the fact of one,
     * is the rule under test.
     */
    public static void checkFatal(Runnable action, String description) {
        try {
            action.run();
            check(false, description + " (expected a fatal fault but nothing was thrown)");
        } catch (LoxError e) {
            check(!e.isCatchable(),
                    description + " (expected a fatal fault but got the catchable one: "
                            + e.getMessage() + ")");
        } catch (Throwable t) {
            check(false, description + " (expected a fatal fault but got "
                    + t.getClass().getSimpleName() + ": " + t.getMessage() + ")");
        }
    }

    /**
     * Asserts that the action raises no fatal Lox++ fault. It may succeed, or
     * raise a catchable one, as a connection attempt does when it is refused.
     */
    public static void checkNotFatal(Runnable action, String description) {
        try {
            action.run();
            check(true, description);
        } catch (LoxError e) {
            check(e.isCatchable(),
                    description + " (expected no fatal fault but got: " + e.getMessage() + ")");
        }
    }

    public static int finish(String suiteName) {
        System.out.println(suiteName + ": " + (checks - failures) + "/" + checks + " checks passed");
        return (failures == 0) ? 0 : 1;
    }
}
