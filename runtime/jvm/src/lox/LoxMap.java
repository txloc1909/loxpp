package lox;

import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;

/**
 * Nil keys are plain {@code null} — {@code java.util.HashMap} (and
 * LinkedHashMap) already accept a null key, so nil needs no sentinel.
 * LinkedHashMap keeps insertion order reproducible across runs. spec/03-
 * types.md leaves map iteration order unspecified; the native ObjMap gives
 * bucket order instead, so the two runtimes may print keys() in different
 * orders on the same program. That gap is not a defect (binding supervisor
 * ruling on PR #97 finding R2, 2026-08-14) and node N11 excludes the
 * order-sensitive examples from the JVM test path for that reason.
 *
 * Key validity (nil/bool/number-not-NaN/string only) is enforced by callers
 * (LoxOps), matching vm.cpp: the check happens at each opcode site, not
 * inside ObjMap itself.
 */
public final class LoxMap {
    private final Map<Object, Object> entries = new LinkedHashMap<>();

    // Every LoxMap lazily grows its own per-instance cache the first time a
    // method is read as a property (not called) — see R3 in PR #97 review:
    // the native VM hands back the same ObjNative on every GET_PROPERTY, so
    // repeated `m.has == m.has` must read the identical Java object twice.
    private final Map<String, LoxCallable> methodCache = new HashMap<>();

    // -0.0 and 0.0 must hash and look up identically, matching IEEE 754
    // numeric equality (value.cpp's hashValue canonicalizes the same way).
    // java.lang.Double.equals/hashCode treat them as distinct, so map keys
    // must be normalized on every access.
    private static Object normalizeKey(Object key) {
        if (key instanceof Double && (Double) key == 0.0) {
            return 0.0;
        }
        return key;
    }

    public void put(Object key, Object value) {
        entries.put(normalizeKey(key), value);
    }

    public Object get(Object key) {
        return entries.get(normalizeKey(key));
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

    public Set<Map.Entry<Object, Object>> entrySet() {
        return entries.entrySet();
    }

    public LoxCallable getMethod(String name) {
        LoxCallable cached = methodCache.get(name);
        if (cached != null) {
            return cached;
        }
        LoxCallable created = createMethod(name);
        if (created != null) {
            methodCache.put(name, created);
        }
        return created;
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
