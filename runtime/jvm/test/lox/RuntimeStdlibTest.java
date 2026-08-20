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
        checkOsAccess(globals);

        System.exit(TestSupport.finish("RuntimeStdlibTest"));
    }

    // Mirrors test/test_os_api.cpp: the OS / world access natives the JVM
    // runtime must expose to match the native VM (see LoxRuntime).
    private static void checkOsAccess(LoxGlobals globals) {
        // args() empty until setProgramArgs is called (the generated main
        // forwards its argv during real runs).
        Object noArgs = call(globals, "args");
        check(noArgs instanceof LoxList && ((LoxList) noArgs).elements.isEmpty(), "args() is empty by default");
        LoxRuntime.setProgramArgs(new String[] {"alpha", "beta"});
        LoxList args = (LoxList) call(globals, "args");
        checkEquals(2, args.elements.size(), "args() reflects setProgramArgs");
        checkEquals("alpha", args.elements.get(0), "args()[0]");
        checkEquals("beta", args.elements.get(1), "args()[1]");

        // env(): present for a real variable, nil for an unknown name.
        check(System.getenv("PATH") != null ? call(globals, "env", "PATH") != null
                : true, "env() reads the environment");
        check(call(globals, "env", "LOXPP_OS_API_NO_SUCH_VAR_XYZ") == null, "env() returns nil for an unknown name");
        checkThrows(() -> call(globals, "env", 42.0), LoxError.class, "env() rejects a non-string");

        // exit(): the terminating path cannot be asserted in-process; the
        // rejection paths can be.
        checkThrows(() -> call(globals, "exit", "no"), LoxError.class, "exit() rejects a non-number");
        checkThrows(() -> call(globals, "exit", 1e300), LoxError.class, "exit() rejects an out-of-range code");
        checkThrows(() -> call(globals, "exit", Double.NaN), LoxError.class, "exit() rejects NaN");

        // time() is wall-clock seconds since the Unix epoch.
        double now = (Double) call(globals, "time");
        check(now > 0, "time() is positive");

        // sleep() returns nil and accepts a fraction.
        check(call(globals, "sleep", 0.0) == null, "sleep(0) returns nil");
        checkThrows(() -> call(globals, "sleep", "long"), LoxError.class, "sleep() rejects a non-number");

        checkFsOsAccess(globals);
    }

    private static void checkFsOsAccess(LoxGlobals globals) {
        try {
            java.io.File tmpDir = java.nio.file.Files.createTempDirectory("lox-rt-os").toFile();
            tmpDir.deleteOnExit();
            java.io.File tmpFile = new java.io.File(tmpDir, "note.txt");
            java.nio.file.Files.write(tmpFile.toPath(), "hello loxpp".getBytes(java.nio.charset.StandardCharsets.ISO_8859_1));
            tmpFile.deleteOnExit();

            check(Boolean.TRUE.equals(call(globals, "exists", tmpFile.getAbsolutePath())), "exists() true for a file");
            check(Boolean.TRUE.equals(call(globals, "exists", tmpDir.getAbsolutePath())), "exists() true for a directory");
            check(Boolean.FALSE.equals(call(globals, "exists", tmpDir.getAbsolutePath() + "/nope.lox")),
                    "exists() false for a missing path");

            check(Boolean.TRUE.equals(call(globals, "is_dir", tmpDir.getAbsolutePath())), "is_dir() true for a directory");
            check(Boolean.FALSE.equals(call(globals, "is_dir", tmpFile.getAbsolutePath())), "is_dir() false for a file");
            check(Boolean.TRUE.equals(call(globals, "is_file", tmpFile.getAbsolutePath())), "is_file() true for a file");
            check(Boolean.FALSE.equals(call(globals, "is_file", tmpDir.getAbsolutePath())), "is_file() false for a directory");

            checkThrows(() -> call(globals, "exists", 1.0), LoxError.class, "exists() rejects a non-string");
            checkThrows(() -> call(globals, "is_dir", 1.0), LoxError.class, "is_dir() rejects a non-string");
            checkThrows(() -> call(globals, "is_file", 1.0), LoxError.class, "is_file() rejects a non-string");

            Object missing = call(globals, "stat", tmpDir.getAbsolutePath() + "/nope.lox");
            check(missing == null, "stat() of a missing path returns nil");

            LoxMap fileStat = (LoxMap) call(globals, "stat", tmpFile.getAbsolutePath());
            check(Boolean.TRUE.equals(fileStat.get("exists")), "stat(file).exists");
            check(Boolean.TRUE.equals(fileStat.get("is_file")), "stat(file).is_file");
            check(Boolean.FALSE.equals(fileStat.get("is_dir")), "stat(file).is_dir");
            checkEquals(11.0, fileStat.get("size"), "stat(file).size");
            check(fileStat.get("mtime") instanceof Double && (Double) fileStat.get("mtime") > 0,
                    "stat(file).mtime is positive");

            LoxMap dirStat = (LoxMap) call(globals, "stat", tmpDir.getAbsolutePath());
            check(Boolean.TRUE.equals(dirStat.get("exists")), "stat(dir).exists");
            check(Boolean.TRUE.equals(dirStat.get("is_dir")), "stat(dir).is_dir");
            check(dirStat.get("size") == null, "stat(dir) has no size key");

            checkThrows(() -> call(globals, "stat", 1.0), LoxError.class, "stat() rejects a non-string");
        } catch (java.io.IOException e) {
            throw new RuntimeException(e);
        }
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
