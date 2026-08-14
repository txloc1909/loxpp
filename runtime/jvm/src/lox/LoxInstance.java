package lox;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * An instance's fields are open-ended (spring into existence on first
 * assignment — spec/03-types.md), so a plain map is the field table; no
 * per-class field layout exists. Identity equality (Lox default for this
 * type) comes for free from Object, so equals/hashCode are left alone.
 */
public final class LoxInstance {
    public final LoxClass klass;
    public final Map<String, Object> fields = new LinkedHashMap<>();

    public LoxInstance(LoxClass klass) {
        this.klass = klass;
    }
}
