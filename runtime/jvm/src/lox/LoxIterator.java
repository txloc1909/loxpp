package lox;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Backs the GET_ITER / ITER_HAS_NEXT / ITER_NEXT protocol. List and String
 * read the live collection by cursor (a growing list is visited further, as
 * in vm.cpp's ObjIterator). A Map snapshots its keys at construction for
 * order, and records the size: a size change during the loop is an error
 * ("Map changed size during iteration."), as in vm.cpp and Python's dict
 * rule. Writing a value to a key that already exists is permitted.
 */
public final class LoxIterator {
    public final Object collection;
    private final List<Object> mapKeys; // non-null only when collection is a LoxMap
    private final int expectedMapSize; // -1 unless collection is a LoxMap
    private int index;

    public LoxIterator(Object collection) {
        this.collection = collection;
        if (collection instanceof LoxMap) {
            mapKeys = new ArrayList<>();
            for (Map.Entry<Object, Object> e : ((LoxMap) collection).entrySet()) {
                mapKeys.add(e.getKey());
            }
            expectedMapSize = ((LoxMap) collection).size();
        } else {
            mapKeys = null;
            expectedMapSize = -1;
        }
    }

    private void checkMapSize() {
        if (mapKeys != null &&
                ((LoxMap) collection).size() != expectedMapSize) {
            throw new LoxError("Map changed size during iteration.");
        }
    }

    public boolean hasNext() {        if (collection instanceof LoxList) {
            return index < ((LoxList) collection).elements.size();
        }
        if (collection instanceof String) {
            return index < ((String) collection).length();
        }
        if (mapKeys != null) {
            checkMapSize();
            return index < mapKeys.size();
        }
        throw new LoxError("BUG: LoxIterator holds an unexpected collection type.");
    }

    /**
     * Requires a preceding true hasNext(): the compiler always emits
     * ITER_HAS_NEXT before ITER_NEXT with no user code between them, so
     * calling next() past the end is unreachable from a valid program.
     */
    public Object next() {
        if (collection instanceof LoxList) {
            return ((LoxList) collection).elements.get(index++);
        }
        if (collection instanceof String) {
            return String.valueOf(((String) collection).charAt(index++));
        }
        if (mapKeys != null) {
            checkMapSize();
            return mapKeys.get(index++);
        }
        throw new LoxError("BUG: LoxIterator holds an unexpected collection type.");
    }
}
