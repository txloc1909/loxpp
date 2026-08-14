package lox;

/**
 * One interface for every callable Lox++ value: closures, classes, enum
 * constructors, bound methods, and natives (P6 — CALL dispatches on the
 * callee's kind, not on one code shape).
 *
 * `args` never carries a slot-0 entry: it holds exactly the Lox arguments the
 * caller pushed (its length equals the callee's declared arity). A method's
 * receiver travels separately — see {@link LoxClosure#callAsSelf}.
 */
public interface LoxCallable {
    Object call(Object[] args);
}
