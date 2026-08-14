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

    public static int finish(String suiteName) {
        System.out.println(suiteName + ": " + (checks - failures) + "/" + checks + " checks passed");
        return (failures == 0) ? 0 : 1;
    }
}
