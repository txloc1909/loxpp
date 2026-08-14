package lox;

import java.util.AbstractMap;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Copies src/core_hash_map.h's CoreHashMap exactly: open addressing, linear
 * probing, tombstones on delete, capacity 0 -> 8 -> doubling at a 0.75 max
 * load. keys()/values()/entries()/stringify()/for-in all read the bucket
 * array in index order, not insertion order — examples/anagram_groups.lox's
 * CHECK lines encode that native bucket order, so any other order fails the
 * example (see PR #97 review finding R2). A fresh CoreHashMap and a fresh
 * LoxMap fed the same keys in the same order land every key in the same
 * bucket, because both use the same FNV-1a string hash (object.h), the same
 * hashValue rule for bool/nil/number (value.cpp), and the same growth
 * schedule.
 *
 * Key validity (nil/bool/number-not-NaN/string only) is enforced by callers
 * (LoxOps), matching vm.cpp: the check happens at each opcode site, not
 * inside ObjMap itself.
 */
public final class LoxMap {
    private static final byte EMPTY = 0;
    private static final byte OCCUPIED = 1;
    private static final byte TOMBSTONE = 2;
    private static final double MAX_LOAD = 0.75;

    private Object[] keys = new Object[0];
    private Object[] values = new Object[0];
    private byte[] states = new byte[0];
    private int count; // live entries
    private int dead; // tombstones

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

    // Mirrors value.cpp's hashValue: bool/nil get fixed hashes, a string
    // hashes by content (object.h's FNV-1a), a number hashes its raw bit
    // pattern folded in half. Java's int wraps on overflow exactly like
    // uint32_t, so the arithmetic needs no masking.
    private static int hashOf(Object key) {
        if (key == null) {
            return 0;
        }
        if (key instanceof Boolean) {
            return ((Boolean) key) ? 1231 : 1237;
        }
        if (key instanceof String) {
            return fnv1a((String) key);
        }
        long bits = Double.doubleToLongBits((Double) key); // key is already normalizeKey'd
        return (int) (bits ^ (bits >>> 32));
    }

    // object.h's hashString: each Lox++ string char is one byte (0-255), a
    // fact the runtime's ISO-8859-1 string boundary guarantees (LoxRuntime).
    private static int fnv1a(String s) {
        int hash = 0x811c9dc5; // 2166136261 as a 32-bit signed int
        for (int i = 0; i < s.length(); i++) {
            hash ^= (s.charAt(i) & 0xFF);
            hash *= 16777619;
        }
        return hash;
    }

    private static boolean keyEquals(Object a, Object b) {
        if (a == null || b == null) {
            return a == b;
        }
        if (a instanceof Double && b instanceof Double) {
            return ((Double) a).doubleValue() == ((Double) b).doubleValue();
        }
        return a.equals(b); // Boolean or String content equality
    }

    /**
     * Copies CoreHashMap::findSlotIn: probe from the key's home bucket until
     * an EMPTY slot ends the chain, returning the matching OCCUPIED slot if
     * one is found, otherwise the first TOMBSTONE seen (so an insert reuses
     * it) or else the terminating EMPTY slot. Callers tell "found" from "not
     * found" by checking the returned slot's state, exactly as the C++ does.
     */
    private int findSlot(Object key, int hash) {
        int cap = keys.length;
        int idx = hash & (cap - 1);
        int tombstone = -1;
        for (;;) {
            byte state = states[idx];
            if (state == EMPTY) {
                return (tombstone != -1) ? tombstone : idx;
            }
            if (state == TOMBSTONE) {
                if (tombstone == -1) {
                    tombstone = idx;
                }
            } else if (keyEquals(keys[idx], key)) {
                return idx;
            }
            idx = (idx + 1) & (cap - 1);
        }
    }

    private int findExisting(Object key) {
        if (keys.length == 0) {
            return -1;
        }
        int idx = findSlot(key, hashOf(key));
        return (states[idx] == OCCUPIED) ? idx : -1;
    }

    private void grow() {
        int cap = keys.length;
        int newCap = (cap < 8) ? 8 : cap * 2;
        Object[] oldKeys = keys;
        Object[] oldValues = values;
        byte[] oldStates = states;
        keys = new Object[newCap];
        values = new Object[newCap];
        states = new byte[newCap]; // defaults to EMPTY (0)

        int saved = 0;
        for (int i = 0; i < oldStates.length; i++) {
            if (oldStates[i] != OCCUPIED) {
                continue;
            }
            int idx = findSlot(oldKeys[i], hashOf(oldKeys[i]));
            keys[idx] = oldKeys[i];
            values[idx] = oldValues[i];
            states[idx] = OCCUPIED;
            saved++;
        }
        count = saved;
        dead = 0;
    }

    public void put(Object key, Object value) {
        key = normalizeKey(key);
        int cap = keys.length;
        if (count + dead + 1 > (int) (cap * MAX_LOAD)) {
            grow();
            cap = keys.length;
        }
        int idx = findSlot(key, hashOf(key));
        boolean wasEmpty = states[idx] == EMPTY;
        boolean wasTombstone = !wasEmpty && states[idx] == TOMBSTONE;
        if (wasEmpty || wasTombstone) {
            count++;
        }
        if (wasTombstone) {
            dead--;
        }
        keys[idx] = key;
        values[idx] = value;
        states[idx] = OCCUPIED;
    }

    public Object get(Object key) {
        int idx = findExisting(normalizeKey(key));
        return (idx == -1) ? null : values[idx];
    }

    public boolean has(Object key) {
        return findExisting(normalizeKey(key)) != -1;
    }

    public void remove(Object key) {
        int idx = findExisting(normalizeKey(key));
        if (idx == -1) {
            return;
        }
        keys[idx] = null;
        values[idx] = null;
        states[idx] = TOMBSTONE;
        count--;
        dead++;
    }

    public int size() {
        return count;
    }

    /** Bucket order (index 0..capacity-1), matching CoreHashMap::forEach. */
    public List<Map.Entry<Object, Object>> entrySet() {
        List<Map.Entry<Object, Object>> result = new ArrayList<>(count);
        for (int i = 0; i < states.length; i++) {
            if (states[i] == OCCUPIED) {
                result.add(new AbstractMap.SimpleImmutableEntry<>(keys[i], values[i]));
            }
        }
        return result;
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
