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
        // PR #97: the stored value is normalized for
        // lookup, but the key keeps the raw bits the caller last wrote.
        // -0.0 prints "0", so the preserved sign bit is not observable
        // through str().
        for (Map.Entry<Object, Object> e : map.entrySet()) {
            if ("neg-zero".equals(e.getValue())) {
                checkEquals("0", LoxOps.stringify(e.getKey()), "a later write under -0.0 stringifies to 0");
            }
        }

        map.remove(null);
        check(!map.has(null), "remove() deletes the key");
        map.remove("never-there"); // no-op, matches ObjMap::mapDel on a missing key
        checkEquals(1, map.size(), "removing an absent key is a no-op");

        // An Int key hashes by value and never equals the equal Number key
        // (spec/03-types.md, Int).
        map.put(1L, "int-one");
        checkEquals("int-one", map.get(1L), "an Int key round-trips");
        checkEquals("neg-zero", map.get(0.0), "an Int key does not replace the Number key");
        map.put(9223372036854775807L, "max");
        checkEquals("max", map.get(9223372036854775807L),
                "an Int key outside the box cache finds its entry");
        checkEquals(3, map.size(), "the Int and Number keys are distinct entries");

        // LinkedHashMap gives insertion order, not the native table's bucket
        // order (PR #97: the gap is
        // legal because spec/03-types.md leaves map order unspecified).
        LoxMap ordered = new LoxMap();
        ordered.put("b", 2.0);
        ordered.put("a", 1.0);
        ordered.put("c", 3.0);
        List<Object> keysInOrder = new ArrayList<>();
        for (Map.Entry<Object, Object> e : ordered.entrySet()) {
            keysInOrder.add(e.getKey());
        }
        checkEquals("[b, a, c]", keysInOrder.toString(), "iteration follows insertion order");

        Object hasMethod = ordered.getMethod("has");
        checkEquals(true, ((LoxCallable) hasMethod).call(new Object[] {"a"}), "map.has via getMethod");
        check(!LoxOps.equal(ordered.getMethod("has"), ordered.getMethod("has")),
                "two reads of the same map's 'has' method are not Lox-equal");
        check(!LoxOps.equal(ordered.getMethod("has"), new LoxMap().getMethod("has")),
                "two different maps' 'has' method values are not Lox-equal");
        Object keysMethod = ordered.getMethod("keys");
        LoxList keys = (LoxList) ((LoxCallable) keysMethod).call(new Object[0]);
        checkEquals(3, keys.elements.size(), "map.keys() returns every key");
        checkEquals("[b, a, c]", keys.elements.toString(), "map.keys() follows insertion order too");
        Object entriesMethod = ordered.getMethod("entries");
        LoxList entries = (LoxList) ((LoxCallable) entriesMethod).call(new Object[0]);
        LoxList firstPair = (LoxList) entries.elements.get(0);
        checkEquals(2, firstPair.elements.size(), "each entries() pair is [key, value]");
        check(ordered.getMethod("nonexistent") == null, "getMethod returns null for an unknown name");

        System.exit(TestSupport.finish("MapTest"));
    }
}
