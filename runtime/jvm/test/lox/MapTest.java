package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public final class MapTest {
    public static void main(String[] args) {
        LoxMap map = new LoxMap();
        checkEquals(0, map.size(), "new map is empty");
        map.put(null, "nil-key"); // nil is a legal map key
        checkEquals("nil-key", map.get(null), "nil key round-trips");
        check(map.has(null), "has() finds a nil key");

        map.put(0.0, "zero");
        checkEquals("zero", map.get(-0.0), "-0.0 and 0.0 look up the same entry");
        map.put(-0.0, "neg-zero");
        checkEquals(2, map.size(), "storing -0.0 after 0.0 overwrites, not inserts");
        checkEquals("neg-zero", map.get(0.0), "the later write under -0.0 is what 0.0 now reads");

        map.remove(null);
        check(!map.has(null), "remove() deletes the key");
        map.remove("never-there"); // no-op, matches ObjMap::mapDel on a missing key
        checkEquals(1, map.size(), "removing an absent key is a no-op");

        LoxMap ordered = new LoxMap();
        ordered.put("b", 2.0);
        ordered.put("a", 1.0);
        ordered.put("c", 3.0);
        List<Object> keysInOrder = new ArrayList<>();
        for (Map.Entry<Object, Object> e : ordered.entrySet()) {
            keysInOrder.add(e.getKey());
        }
        checkEquals("[b, a, c]", keysInOrder.toString(), "LinkedHashMap keeps insertion order");

        Object hasMethod = ordered.getMethod("has");
        checkEquals(true, ((LoxCallable) hasMethod).call(new Object[] {"a"}), "map.has via getMethod");
        Object keysMethod = ordered.getMethod("keys");
        LoxList keys = (LoxList) ((LoxCallable) keysMethod).call(new Object[0]);
        checkEquals(3, keys.elements.size(), "map.keys() returns every key");
        Object entriesMethod = ordered.getMethod("entries");
        LoxList entries = (LoxList) ((LoxCallable) entriesMethod).call(new Object[0]);
        LoxList firstPair = (LoxList) entries.elements.get(0);
        checkEquals(2, firstPair.elements.size(), "each entries() pair is [key, value]");
        check(ordered.getMethod("nonexistent") == null, "getMethod returns null for an unknown name");

        System.exit(TestSupport.finish("MapTest"));
    }
}
