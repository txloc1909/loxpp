package lox;

import java.util.AbstractMap;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Key validity is enforced by callers (LoxOps), matching vm.cpp.
 *
 * Entries live in a hash-bucketed table: one bucket per key hash, each bucket
 * a list probed in insertion order. This matches the native ObjMap's own
 * "find the bucket from the key's hash, then compare the stored key" probe,
 * so an Instance key and a scalar key that share a bucket resolve the same way
 * on both runtimes. Scalar and String keys use their own hash; an Instance key
 * calls __hash__ (LoxOps.hashKey), and a collision is resolved with the stored
 * key's __eq__ (LoxOps.keyEquals, stored key on the left). spec/03-types.md
 * leaves map iteration order unspecified, so buckets need no set order beyond
 * insertion order within one bucket.
 */
public final class LoxMap {
    private static final class Slot {
        final Object displayKey;
        final Object value;

        Slot(Object displayKey, Object value) {
            this.displayKey = displayKey;
            this.value = value;
        }
    }

    private final Map<Integer, List<Slot>> buckets = new LinkedHashMap<>();
    private int size;

    // Structural version, bumped on a real insert or a real erase only.
    // A repeat write of one key and a remove of a missing key leave it
    // alone. LoxIterator snapshots it at construction, so a paired erase
    // plus insert that restores the net size still trips the check.
    private int version;

    // Non-zero while a key operation runs. put/remove then raise
    // MapChangedError, so a user __hash__/__eq__ cannot mutate this map
    // during a lookup.
    private int keyOpDepth;

    /** Structural version for the for-in fail-fast check. */
    int version() {
        return version;
    }

    private List<Slot> bucketFor(int hash) {
        return buckets.computeIfAbsent(hash, k -> new ArrayList<>());
    }

    public void put(Object key, Object value) {
        if (keyOpDepth > 0) {
            throw LoxOps.makeError("MapChangedError",
                                   "Map changed during key hashing or equality.");
        }
        keyOpDepth++;
        try {
            List<Slot> bucket = bucketFor(LoxOps.hashKey(key));
            for (int i = 0; i < bucket.size(); i++) {
                if (LoxOps.keyEquals(bucket.get(i).displayKey, key)) {
                    bucket.set(i, new Slot(key, value));
                    return;
                }
            }
            bucket.add(new Slot(key, value));
            size++;
            version++;
        } finally {
            keyOpDepth--;
        }
    }

    public Object get(Object key) {
        keyOpDepth++;
        try {
            List<Slot> bucket = buckets.get(LoxOps.hashKey(key));
            if (bucket != null) {
                for (Slot slot : bucket) {
                    if (LoxOps.keyEquals(slot.displayKey, key)) {
                        return slot.value;
                    }
                }
            }
            return null;
        } finally {
            keyOpDepth--;
        }
    }

    public boolean has(Object key) {
        keyOpDepth++;
        try {
            List<Slot> bucket = buckets.get(LoxOps.hashKey(key));
            if (bucket != null) {
                for (Slot slot : bucket) {
                    if (LoxOps.keyEquals(slot.displayKey, key)) {
                        return true;
                    }
                }
            }
            return false;
        } finally {
            keyOpDepth--;
        }
    }

    public void remove(Object key) {
        if (keyOpDepth > 0) {
            throw LoxOps.makeError("MapChangedError",
                                   "Map changed during key hashing or equality.");
        }
        keyOpDepth++;
        try {
            int hash = LoxOps.hashKey(key);
            List<Slot> bucket = buckets.get(hash);
            if (bucket != null) {
                for (int i = 0; i < bucket.size(); i++) {
                    if (LoxOps.keyEquals(bucket.get(i).displayKey, key)) {
                        bucket.remove(i);
                        size--;
                        version++;
                        if (bucket.isEmpty()) {
                            buckets.remove(hash);
                        }
                        return;
                    }
                }
            }
        } finally {
            keyOpDepth--;
        }
    }

    public int size() {
        return size;
    }

    /** Insertion order within each bucket, each key as the caller last wrote it. */
    public List<Map.Entry<Object, Object>> entrySet() {
        List<Map.Entry<Object, Object>> result = new ArrayList<>(size);
        for (List<Slot> bucket : buckets.values()) {
            for (Slot slot : bucket) {
                result.add(new AbstractMap.SimpleImmutableEntry<>(
                    slot.displayKey, slot.value));
            }
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
        LoxCallable unbound = createMethod(name);
        if (unbound == null) {
            return null;
        }
        // Every branch of createMethod returns a LoxNative; re-wrap it with
        // this map as its receiver so type() can tell a bound Map method
        // apart from an ordinary, unbound native — see LoxNative.receiver.
        LoxNative n = (LoxNative) unbound;
        return new LoxNative(n.name, n.arity, n::call, this);
    }

    private LoxCallable createMethod(String name) {
        switch (name) {
        case "has":
            return new LoxNative("has", 1, a -> {
                LoxOps.checkMapKeyForNativeMethod(a[0]);
                return has(a[0]);
            });
        case "del":
            return new LoxNative("del", 1, a -> {
                LoxOps.checkMapKeyForNativeMethod(a[0]);
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
