package lox;

/** A Lox++ runtime error. Uncaught, it aborts the program (see vm.cpp runtimeError). */
public final class LoxError extends RuntimeException {
    public LoxError(String message) {
        super(message);
    }
}
