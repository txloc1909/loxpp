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
     * True if this fault reaches a live Lox try/catch (and, on the
     * exceptional-exit path, a function's own pending defers — issue #319):
     * mirrors runtime/clr/src/LoxError.cs's Catchable property, and
     * src/vm.cpp's split between a fault raised through
     * tryCatchableError/raiseThrowableError (reaches handleThrow) and one
     * raised through RAISE_ERROR/plain runtimeError (never does). Unlike
     * {@link #getValue()}, this has no side effect, so callers that only
     * need the flag — not the wrapped value — can check it without
     * triggering getValue()'s own rethrow-if-uncatchable behavior.
     */
    public boolean isCatchable() {
        return value != null;
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
        if (LoxClosure.isOverflowInFlight(this)) {
            // This is the fault LoxClosure is watching for (the
            // StackOverflowError it raised, or whatever replaced it — see
            // LoxClosure.s_overflowInFlight's own comment), and it is about
            // to reach a real catchBlock, so that unwind is over.
            LoxClosure.endStackOverflowUnwind();
        }
        return value;
    }
}
