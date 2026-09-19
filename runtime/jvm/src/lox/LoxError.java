package lox;

/**
 * A Lox++ runtime error. Uncaught, it aborts the program (see vm.cpp
 * runtimeError).
 */
public final class LoxError extends RuntimeException {
    /**
     * The Lox++ value being thrown; null for an internally-raised fault
     * (caught as an Error instance).
     */
    private final Object value;

    /** Construct a LoxError with a message string (fault, not throw). */
    public LoxError(String message) {
        super(message);
        this.value = null;
    }

    /** Construct a LoxError carrying a user-thrown Lox++ value. */
    public LoxError(String message, Object value) {
        super(message);
        this.value = value;
    }

    /**
     * Get the Lox++ value being thrown. Returns the original value if this
     * error wraps a user throw. If this is an internally-raised fault
     * (value == null), rethrow the exception instead of returning it,
     * making internally-raised faults uncatchable by Lox try/catch blocks.
     * This is the mechanism that distinguishes catchable errors (from user
     * throw or makeError() with an Error instance) from fatal system faults.
     */
    public Object getValue() {
        if (value == null) {
            throw this;
        }
        if (value instanceof LoxInstance
                && "StackOverflowError".equals(((LoxInstance)value).fields.get("kind"))) {
            // This fault is about to reach a real catchBlock, so the unwind
            // LoxClosure started when it raised it is over — see
            // LoxClosure.endStackOverflowUnwind()'s own comment.
            LoxClosure.endStackOverflowUnwind();
        }
        return value;
    }
}
