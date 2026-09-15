package lox;

/**
 * Captures a callable and its arguments for deferred execution.
 * Represents a single `defer` statement from Lox++.
 */
public class DeferredCall {
    public final Object callable;     // Closure or native function
    public final Object[] args;       // Argument values

    public DeferredCall(Object callable, Object[] args) {
        this.callable = callable;
        this.args = args;
    }
}
