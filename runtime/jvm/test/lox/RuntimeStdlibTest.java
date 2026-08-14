package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

public final class RuntimeStdlibTest {
    private static Object call(LoxGlobals g, String name, Object... args) {
        return ((LoxCallable) g.get(name)).call(args);
    }

    public static void main(String[] args) {
        LoxGlobals globals = LoxRuntime.init();

        check(call(globals, "clock") instanceof Double, "clock() returns a number");
        checkEquals("42", call(globals, "str", 42.0), "str(42)");
        checkEquals("true", call(globals, "str", true), "str(true)");
        checkEquals(3.0, call(globals, "len", listOf(1.0, 2.0, 3.0)), "len(list)");
        checkEquals(5.0, call(globals, "len", "hello"), "len(string)");
        checkThrows(() -> call(globals, "len", true), LoxError.class, "len() rejects a non-sequence");

        Object math = globals.get("math");
        check(math instanceof LoxInstance, "math is an instance");
        checkEquals(4.0, callField((LoxInstance) math, "abs", -4.0), "math.abs(-4)");
        checkEquals(2.0, callField((LoxInstance) math, "sqrt", 4.0), "math.sqrt(4)");
        checkEquals(-1.0, callField((LoxInstance) math, "round", -0.5), "math.round(-0.5) rounds away from zero");
        checkEquals(1.0, callField((LoxInstance) math, "round", 0.5), "math.round(0.5) rounds away from zero");
        checkEquals(5.0, ((LoxCallable) ((LoxInstance) math).fields.get("min"))
                .call(new Object[] {Double.NaN, 5.0}), "math.min ignores a NaN operand");
        check((Double) ((LoxInstance) math).fields.get("pi") > 3.14, "math.pi is present");

        checkOpenRoundtrip(globals);

        System.exit(TestSupport.finish("RuntimeStdlibTest"));
    }

    private static void checkOpenRoundtrip(LoxGlobals globals) {
        try {
            java.io.File tmp = java.io.File.createTempFile("lox-rt-stdlib-open", ".txt");
            tmp.deleteOnExit();
            Object file = call(globals, "open", tmp.getAbsolutePath(), "w");
            check(file instanceof LoxFile, "open() returns a LoxFile");
            ((LoxFile) file).close();
            checkThrows(() -> call(globals, "open", 1.0, "w"), LoxError.class,
                    "open() requires string arguments");
        } catch (java.io.IOException e) {
            throw new RuntimeException(e);
        }
    }

    private static Object callField(LoxInstance instance, String name, Object... args) {
        return ((LoxCallable) instance.fields.get(name)).call(args);
    }

    private static LoxList listOf(Object... items) {
        LoxList list = new LoxList();
        for (Object item : items) {
            list.elements.add(item);
        }
        return list;
    }
}
