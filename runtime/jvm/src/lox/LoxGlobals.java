package lox;

import java.util.HashMap;
import java.util.Map;

/**
 * Globals are one dynamic name-to-value map (design decision A2), not one
 * static field per global — GET_GLOBAL/SET_GLOBAL are late-bound by name at
 * every access, exactly as in the native VM's hash-table globals.
 */
public final class LoxGlobals {
    private final Map<String, Object> values = new HashMap<>();

    public void define(String name, Object value) {
        values.put(name, value);
    }

    public Object get(String name) {
        if (!values.containsKey(name)) {
            throw new LoxError("Undefined variable '" + name + "'.");
        }
        return values.get(name);
    }

    public void set(String name, Object value) {
        if (!values.containsKey(name)) {
            throw new LoxError("Undefined variable '" + name + "'.");
        }
        values.put(name, value);
    }

    /** Non-throwing existence check — INSTANCEOF looks up its class name this way, not via get(). */
    public boolean isDefined(String name) {
        return values.containsKey(name);
    }
}
