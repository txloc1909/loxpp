package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

import java.io.File;
import java.io.IOException;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/** LoxRuntime.registerReflection — type/fields/methods/getField/setField/hasField/callMethod. */
public final class ReflectionTest {
    private static Object call(LoxGlobals g, String name, Object... args) {
        return ((LoxCallable) g.get(name)).call(args);
    }

    private static LoxClosure noopClosure(String name, int arity) {
        return new LoxClosure(name, arity, new Object[0][]) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                return "ran:" + name;
            }
        };
    }

    public static void main(String[] args) {
        LoxGlobals globals = LoxRuntime.init();

        checkType(globals);
        checkFieldsAndMethods(globals);
        checkFieldAccessors(globals);
        checkCallMethod(globals);

        System.exit(TestSupport.finish("ReflectionTest"));
    }

    private static void checkType(LoxGlobals globals) {
        checkEquals("Nil", call(globals, "type", new Object[] {null}), "type(nil)");
        checkEquals("Boolean", call(globals, "type", true), "type(boolean)");
        checkEquals("Number", call(globals, "type", 1.0), "type(number)");
        checkEquals("String", call(globals, "type", "s"), "type(string)");

        LoxClosure fn = noopClosure("f", 0);
        checkEquals("Function", call(globals, "type", fn), "type(closure) is Function");
        checkEquals("Function", call(globals, "type", globals.get("str")), "type(plain native) is Function");

        LoxClass klass = new LoxClass("Foo", null);
        LoxOps.defineMethod(klass, "greet", noopClosure("greet", 0));
        Object instance = klass.call(new Object[0]);
        Object userBound = LoxOps.getProperty(instance, "greet");
        check(userBound instanceof LoxBoundMethod, "sanity: GET_PROPERTY on a method yields a bound method");
        checkEquals("BoundMethod", call(globals, "type", userBound), "type(user-defined bound method)");

        LoxMap map = new LoxMap();
        Object mapBoundNative = LoxOps.getProperty(map, "has");
        check(mapBoundNative instanceof LoxNative, "a Map method read via property access is a LoxNative");
        checkEquals("BoundMethod", call(globals, "type", mapBoundNative),
                "type(map bound native) is BoundMethod, not Function");

        checkEquals("Class", call(globals, "type", klass), "type(class)");
        checkEquals("Instance", call(globals, "type", instance), "type(instance)");
        checkEquals("List", call(globals, "type", new LoxList()), "type(list)");
        checkEquals("Map", call(globals, "type", map), "type(map)");

        LoxIterator it = LoxOps.getIter(new LoxList());
        checkEquals("Iterator", call(globals, "type", it), "type(iterator)");

        LoxEnumCtor ctor = new LoxEnumCtor(0, 0, "Green", "Color");
        checkEquals("EnumConstructor", call(globals, "type", ctor), "type(enum constructor)");
        Object enumVal = ctor.call(new Object[0]);
        checkEquals("Enum", call(globals, "type", enumVal), "type(enum value)");

        try {
            File tmp = File.createTempFile("lox-rt-reflection", ".txt");
            tmp.deleteOnExit();
            LoxFile file = LoxFile.open(tmp.getAbsolutePath(), "w");
            checkEquals("File", call(globals, "type", file), "type(file)");
            Object fileBoundNative = LoxOps.getProperty(file, "write");
            check(fileBoundNative instanceof LoxNative, "a File method read via property access is a LoxNative");
            checkEquals("BoundMethod", call(globals, "type", fileBoundNative),
                    "type(file bound native) is BoundMethod, not Function");
            file.close();
        } catch (IOException e) {
            throw new RuntimeException(e);
        }
    }

    private static void checkFieldsAndMethods(LoxGlobals globals) {
        LoxClass animal = new LoxClass("Animal", null);
        LoxOps.defineMethod(animal, "speak", noopClosure("speak", 0));
        LoxClass dog = new LoxClass("Dog", animal);
        LoxOps.defineMethod(dog, "bark", noopClosure("bark", 0));

        checkEquals(setOf("bark", "speak"), setOf(toArray((LoxList) call(globals, "methods", dog))),
                "methods(dog) includes its own and inherited methods");

        Object instance = dog.call(new Object[0]);
        LoxOps.setProperty(instance, "name", "Rex");
        LoxOps.setProperty(instance, "age", 3.0);
        checkEquals(setOf("name", "age"), setOf(toArray((LoxList) call(globals, "fields", instance))),
                "fields(instance) lists exactly the fields that were set");

        checkThrows(() -> call(globals, "fields", 1.0), LoxError.class, "fields() rejects a non-instance");
        checkThrows(() -> call(globals, "methods", 1.0), LoxError.class, "methods() rejects a non-class");
    }

    private static void checkFieldAccessors(LoxGlobals globals) {
        LoxClass klass = new LoxClass("Foo", null);
        LoxOps.defineMethod(klass, "greet", noopClosure("greet", 0));
        Object instance = klass.call(new Object[0]);

        // Fields-only: a method by the same name never counts, unlike `.` access.
        check(Boolean.FALSE.equals(call(globals, "hasField", instance, "greet")),
                "hasField() ignores a method of the same name");
        checkEquals(null, call(globals, "getField", instance, "greet"),
                "getField() returns nil for a name that is only a method");

        checkEquals(null, call(globals, "getField", instance, "missing"), "getField() of an absent field is nil");
        check(Boolean.FALSE.equals(call(globals, "hasField", instance, "missing")), "hasField() of an absent field is false");

        checkEquals(42.0, call(globals, "setField", instance, "x", 42.0), "setField() returns the assigned value");
        check(Boolean.TRUE.equals(call(globals, "hasField", instance, "x")), "hasField() true after setField()");
        checkEquals(42.0, call(globals, "getField", instance, "x"), "getField() reads back what setField() wrote");

        // Overwrite, not duplicate.
        call(globals, "setField", instance, "x", 43.0);
        checkEquals(43.0, call(globals, "getField", instance, "x"), "setField() overwrites an existing field");
        checkEquals(1, ((LoxList) call(globals, "fields", instance)).elements.size(),
                "overwriting a field does not add a second entry");

        checkThrows(() -> call(globals, "getField", 1.0, "x"), LoxError.class, "getField() rejects a non-instance");
        checkThrows(() -> call(globals, "hasField", 1.0, "x"), LoxError.class, "hasField() rejects a non-instance");
        checkThrows(() -> call(globals, "setField", 1.0, "x", 1.0), LoxError.class, "setField() rejects a non-instance");
        checkThrows(() -> call(globals, "getField", instance, 1.0), LoxError.class, "getField() rejects a non-string name");
    }

    private static void checkCallMethod(LoxGlobals globals) {
        LoxClass klass = new LoxClass("Foo", null);
        LoxOps.defineMethod(klass, "greet", noopClosure("greet", 0));
        Object instance = klass.call(new Object[0]);

        // A plain native stored in a field.
        LoxOps.setProperty(instance, "describe", globals.get("str"));
        checkEquals("42", call(globals, "callMethod", instance, "describe", 42.0),
                "callMethod() calls a plain native stored in a field");

        // A native with a real arity, to prove args forward in order.
        LoxNative addTwo = new LoxNative("addTwo", 2, a -> (Double) a[0] + (Double) a[1]);
        LoxOps.setProperty(instance, "addTwo", addTwo);
        checkEquals(7.0, call(globals, "callMethod", instance, "addTwo", 3.0, 4.0),
                "callMethod() forwards trailing args in order");

        // The trickiest case: a Map/File bound native stored in a field must
        // run against ITS OWN receiver, never against callMethod's `instance`
        // argument (the historical native-VM bug this mirrors: substituting
        // the wrong receiver into the forwarded-args slot).
        LoxMap mapA = new LoxMap();
        mapA.put("k", "present-in-a");
        LoxMap mapB = new LoxMap();
        LoxOps.setProperty(instance, "hasOnA", LoxOps.getProperty(mapA, "has"));
        LoxOps.setProperty(instance, "hasOnB", LoxOps.getProperty(mapB, "has"));
        check(Boolean.TRUE.equals(call(globals, "callMethod", instance, "hasOnA", "k")),
                "callMethod() on a bound map native queries its OWN map (found)");
        check(Boolean.FALSE.equals(call(globals, "callMethod", instance, "hasOnB", "k")),
                "callMethod() on a different bound map native queries ITS OWN map, not the other one");

        // A method resolved from the class (not a field) is closure-backed:
        // v1 restriction, matches native and CLR.
        checkThrows(() -> call(globals, "callMethod", instance, "greet"), LoxError.class,
                "callMethod() rejects a resolved user-defined method");

        // A field holding a closure is the same restriction.
        LoxOps.setProperty(instance, "closureField", noopClosure("g", 0));
        checkThrows(() -> call(globals, "callMethod", instance, "closureField"), LoxError.class,
                "callMethod() rejects a field holding a closure");

        // A field holding a bound user-defined method, same restriction.
        LoxClass other = new LoxClass("Other", null);
        LoxOps.defineMethod(other, "m", noopClosure("m", 0));
        Object otherInstance = other.call(new Object[0]);
        LoxOps.setProperty(instance, "boundMethodField", LoxOps.getProperty(otherInstance, "m"));
        checkThrows(() -> call(globals, "callMethod", instance, "boundMethodField"), LoxError.class,
                "callMethod() rejects a field holding a bound user-defined method");

        // Field shadows method: a field with the same name as a method wins.
        LoxOps.setProperty(instance, "greet", globals.get("str"));
        checkEquals("1", call(globals, "callMethod", instance, "greet", 1.0),
                "callMethod() resolves a shadowing field before the class method");

        checkThrows(() -> call(globals, "callMethod", instance, "nonexistent"), LoxError.class,
                "callMethod() rejects an undefined name");
        checkThrows(() -> call(globals, "callMethod", instance, "describe"), LoxError.class,
                "callMethod() propagates the resolved native's own arity check");
        checkThrows(() -> call(globals, "callMethod", instance), LoxError.class,
                "callMethod() requires at least 2 arguments (missing the name)");
        checkThrows(() -> call(globals, "callMethod", 1.0, "x"), LoxError.class,
                "callMethod() rejects a non-instance receiver");
    }

    private static Object[] toArray(LoxList list) {
        return list.elements.toArray();
    }

    private static Set<Object> setOf(Object... items) {
        return new HashSet<>(Arrays.asList(items));
    }
}
