package lox;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Backs the GET_ITER / ITER_HAS_NEXT / ITER_NEXT protocol. List and String
 * read the live collection by cursor (a growing list is visited further, as
 * in vm.cpp's ObjIterator). A Map instead snapshots its keys at construction:
 * LinkedHashMap's own iterator is fail-fast and would throw on a concurrent
 * `del`, and map-mutation-during-iteration is unspecified by the spec anyway,
 * so a snapshot is a safe, deterministic choice.
 */
public final class LoxIterator {
    public final Object collection;
    private final List<Object> mapKeys; // non-null only when collection is a LoxMap
    private int index;

    public LoxIterator(Object collection) {
        this.collection = collection;
        if (collection instanceof LoxMap) {
            mapKeys = new ArrayList<>();
            for (Map.Entry<Object, Object> e : ((LoxMap) collection).entrySet()) {
                mapKeys.add(e.getKey());
            }
        } else {
            mapKeys = null;
        }
    }

    public boolean hasNext() {
        if (collection instanceof LoxList) {
            return index < ((LoxList) collection).elements.size();
        }
        if (collection instanceof String) {
            return index < ((String) collection).length();
        }
        if (mapKeys != null) {
            return index < mapKeys.size();
        }
        throw new LoxError("BUG: LoxIterator holds an unexpected collection type.");
    }

    public Object next() {
        if (collection instanceof LoxList) {
            return ((LoxList) collection).elements.get(index++);
        }
        if (collection instanceof String) {
            return String.valueOf(((String) collection).charAt(index++));
        }
        if (mapKeys != null) {
            return mapKeys.get(index++);
        }
        throw new LoxError("BUG: LoxIterator holds an unexpected collection type.");
    }
}
