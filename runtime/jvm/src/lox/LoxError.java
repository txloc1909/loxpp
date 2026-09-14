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
     * error wraps a user throw; null if this is an internally-raised fault.
     */
    public Object getValue() { return value; }
}
