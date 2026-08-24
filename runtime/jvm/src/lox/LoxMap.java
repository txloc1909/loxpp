package lox;

import java.util.AbstractMap;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Nil keys are plain {@code null} — {@code java.util.HashMap} (and
 * LinkedHashMap) already accept a null key, so nil needs no sentinel.
 * LinkedHashMap keeps insertion order reproducible across runs. spec/03-
 * types.md leaves map iteration order unspecified; the native ObjMap gives
 * bucket order instead, so the two runtimes may print keys() in different
 * orders on the same program. That gap is not a defect (maintainer ruling,
 * 2026-08-14); the order-sensitive examples are excluded from the JVM
 * differential test path for that reason (see tools/jvm_excluded_examples.txt).
 *
 * Key validity (nil/bool/number-not-NaN/string only) is enforced by callers
 * (LoxOps), matching vm.cpp: the check happens at each opcode site, not
 * inside ObjMap itself.
 */
public final class LoxMap {
    // Keyed by the numerically-normalized key, so -0.0 and 0.0 land in the
    // same slot (matching value.cpp's hashValue). Each Slot separately holds
    // the exact key object the caller last wrote: CoreHashMap::set replaces
    // the whole entry, key included, on a repeat write, not only the value,
    // so a later write under -0.0 must still print as -0, even though it
    // looks up the same slot as 0.0.
    private static final class Slot {
        final Object displayKey;
        final Object value;

        Slot(Object displayKey, Object value) {
            this.displayKey = displayKey;
            this.value = value;
        }
    }

    private final Map<Object, Slot> entries = new LinkedHashMap<>();

    // -0.0 and 0.0 must hash and look up identically, matching IEEE 754
    // numeric equality (value.cpp's hashValue canonicalizes the same way).
    // java.lang.Double.equals/hashCode treat them as distinct, so lookups
    // must normalize the key. This is a lookup-only concern: the Slot above
    // still remembers the un-normalized key for display.
    private static Object normalizeKey(Object key) {
        if (key instanceof Double && (Double) key == 0.0) {
            return 0.0;
        }
        return key;
    }

    public void put(Object key, Object value) {
        entries.put(normalizeKey(key), new Slot(key, value));
    }

    public Object get(Object key) {
        Slot slot = entries.get(normalizeKey(key));
        return (slot == null) ? null : slot.value;
    }

    public boolean has(Object key) {
        return entries.containsKey(normalizeKey(key));
    }

    public void remove(Object key) {
        entries.remove(normalizeKey(key));
    }

    public int size() {
        return entries.size();
    }

    /** Insertion order, each entry's key exactly as the caller last wrote it. */
    public List<Map.Entry<Object, Object>> entrySet() {
        List<Map.Entry<Object, Object>> result = new ArrayList<>(entries.size());
        for (Slot slot : entries.values()) {
            result.add(new AbstractMap.SimpleImmutableEntry<>(slot.displayKey, slot.value));
        }
        return result;
    }

    /**
     * A fresh method value on every call, matching src/vm.cpp's
     * Op::GET_PROPERTY, which wraps a new ObjBoundNative on every read
     * (the isMap branch). No two reads give the same object, so
     * LoxOps.equal's reference-identity rule gives false for a repeat read
     * of one map and for a read of two different maps.
     */
    public LoxCallable getMethod(String name) {
        return createMethod(name);
    }

    private LoxCallable createMethod(String name) {
        switch (name) {
        case "has":
            return new LoxNative("has", 1, a -> {
                LoxOps.checkMapKey(a[0]);
                return has(a[0]);
            });
        case "del":
            return new LoxNative("del", 1, a -> {
                LoxOps.checkMapKey(a[0]);
                remove(a[0]);
                return null;
            });
        case "keys":
            return new LoxNative("keys", 0, a -> {
                LoxList list = new LoxList();
                for (Map.Entry<Object, Object> e : entrySet()) {
                    list.elements.add(e.getKey());
                }
                return list;
            });
        case "values":
            return new LoxNative("values", 0, a -> {
                LoxList list = new LoxList();
                for (Map.Entry<Object, Object> e : entrySet()) {
                    list.elements.add(e.getValue());
                }
                return list;
            });
        case "entries":
            return new LoxNative("entries", 0, a -> {
                LoxList list = new LoxList();
                for (Map.Entry<Object, Object> e : entrySet()) {
                    LoxList pair = new LoxList();
                    pair.elements.add(e.getKey());
                    pair.elements.add(e.getValue());
                    list.elements.add(pair);
                }
                return list;
            });
        default:
            return null;
        }
    }
}
