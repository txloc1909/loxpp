package lox;

import java.util.HashMap;
import java.util.Map;

/**
 * Globals are one dynamic name-to-value map (design decision A2), not one
 * static field per global — GET_GLOBAL/SET_GLOBAL are late-bound by name at
 * every access, exactly as in the native VM's hash-table globals.
 */
public final class LoxGlobals {
    // Stands in for Lox nil inside the table, so a stored entry is never
    // real Java null. That makes "absent" (values.get == null) and "defined
    // as nil" distinguishable from one hashCode/equals lookup, instead of a
    // containsKey call followed by a second get call — GET_GLOBAL is the
    // hottest opcode in the self-hosted interpreter.
    private static final Object NIL = new Object();
    private final Map<String, Object> values = new HashMap<>();

    public void define(String name, Object value) {
        values.put(name, box(value));
    }

    public Object get(String name) {
        Object v = values.get(name);
        if (v == null) {
            throw new LoxError("Undefined variable '" + name + "'.");
        }
        return unbox(v);
    }

    /** Matches vm.cpp's SET_GLOBAL: write speculatively, then undo and throw if the name was never defined. */
    public void set(String name, Object value) {
        Object prev = values.put(name, box(value));
        if (prev == null) {
            values.remove(name);
            throw new LoxError("Undefined variable '" + name + "'.");
        }
    }

    /** Non-throwing existence check — INSTANCEOF looks up its class name this way, not via get(). */
    public boolean isDefined(String name) {
        return values.get(name) != null;
    }

    private static Object box(Object loxValue) {
        return (loxValue == null) ? NIL : loxValue;
    }

    private static Object unbox(Object stored) {
        return (stored == NIL) ? null : stored;
    }
}
