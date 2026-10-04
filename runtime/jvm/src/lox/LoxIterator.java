package lox;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * Backs the GET_ITER / ITER_HAS_NEXT / ITER_NEXT protocol. List and String
 * read the live collection by cursor (a growing list is visited further, as
 * in vm.cpp's ObjIterator). A Map snapshots its keys at construction for
 * order, and records the structural version: any insert or erase during
 * the loop is an error ("Map changed size during iteration."), as in
 * vm.cpp and Python's dict rule — even a paired erase plus insert that
 * restores the net size. Writing a value to a key that already exists is
 * permitted.
 */
public final class LoxIterator {
    public final Object collection;
    private final List<Object> mapKeys; // non-null only when collection is a LoxMap
    private final int expectedMapVersion; // -1 unless collection is a LoxMap
    private int index;
    // Coroutine mode only: the value ITER_HAS_NEXT resumed out of the
    // coroutine, cached for ITER_NEXT to push. The resume happens at has-next
    // time because its outcome is what decides whether another element exists
    // at all.
    private Object current;

    public LoxIterator(Object collection) {
        this.collection = collection;
        if (collection instanceof LoxMap) {
            mapKeys = new ArrayList<>();
            for (Map.Entry<Object, Object> e : ((LoxMap) collection).entrySet()) {
                mapKeys.add(e.getKey());
            }
            expectedMapVersion = ((LoxMap) collection).version();
        } else {
            mapKeys = null;
            expectedMapVersion = -1;
        }
    }

    private void checkMapVersion() {
        if (mapKeys != null &&
                ((LoxMap) collection).version() != expectedMapVersion) {
            throw new LoxError("Map changed size during iteration.");
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
            checkMapVersion();
            return index < mapKeys.size();
        }
        // spec/04-semantics.md for-in, Coroutine row: an already-dead
        // coroutine ends the loop with no resume attempt; otherwise the
        // resume itself decides — a yield supplies the next element, a return
        // (the function finished) ends the loop. The yielded value is cached
        // because the resume must not run twice.
        if (collection instanceof LoxCoroutine) {
            LoxCoroutine co = (LoxCoroutine) collection;
            if (co.state() == LoxCoroutine.State.DEAD) {
                return false;
            }
            Object yielded = co.resume(new Object[0]);
            if (co.state() == LoxCoroutine.State.SUSPENDED) {
                current = yielded;
                return true;
            }
            return false;
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
            checkMapVersion();
            return mapKeys.get(index++);
        }
        // ITER_HAS_NEXT already resumed the coroutine and cached the yielded
        // value; no second resume happens here.
        if (collection instanceof LoxCoroutine) {
            Object v = current;
            current = null;
            return v;
        }
        throw new LoxError("BUG: LoxIterator holds an unexpected collection type.");
    }
}
