package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkFatal;
import static lox.TestSupport.checkNotFatal;
import static lox.TestSupport.checkThrows;

public final class RuntimeStdlibTest {
    private static Object call(LoxGlobals g, String name, Object... args) {
        return ((LoxCallable) g.get(name)).call(args);
    }

    public static void main(String[] args) {
        LoxGlobals globals = LoxRuntime.init();

        check(call(globals, "clock") instanceof Double, "clock() returns a number");
        checkEquals("42", LoxOps.str(42.0), "str(42)");
        checkEquals("true", LoxOps.str(true), "str(true)");
        checkEquals(3.0, LoxOps.len(listOf(1.0, 2.0, 3.0)), "len(list)");
        checkEquals(5.0, LoxOps.len("hello"), "len(string)");
        checkThrows(() -> LoxOps.len(true), LoxError.class, "len() rejects a non-sequence");

        // ord()/chr() and String byte ordering (issue #507).
        checkEquals(65.0, call(globals, "ord", "A"), "ord(\"A\")");
        checkEquals("B", call(globals, "chr", 66.0), "chr(66)");
        checkEquals(255.0, call(globals, "ord", call(globals, "chr", 255.0)),
                "ord(chr(255)) round-trips");
        checkThrows(() -> call(globals, "ord", ""), LoxError.class, "ord() rejects an empty string");
        checkThrows(() -> call(globals, "ord", "ab"), LoxError.class, "ord() rejects two characters");
        checkThrows(() -> call(globals, "chr", 256.0), LoxError.class, "chr() rejects 256");
        checkThrows(() -> call(globals, "chr", 1.5), LoxError.class, "chr() rejects a fraction");
        check(LoxOps.less("a", "b"), "less orders strings");
        check(LoxOps.greater("b", "a"), "greater orders strings");
        check(LoxOps.less("ab", "abc"), "a prefix orders before the longer string");
        check(!LoxOps.less("b", "a"), "less is false when the left operand is larger");
        check(LoxOps.greater("\u00ff", "\u0001"), "a high byte sorts after a low byte");

        Object math = globals.get("math");
        check(math instanceof LoxInstance, "math is an instance");
        checkEquals(4.0, callField((LoxInstance) math, "abs", -4.0), "math.abs(-4)");
        checkEquals(2.0, callField((LoxInstance) math, "sqrt", 4.0), "math.sqrt(4)");
        checkEquals(-1.0, callField((LoxInstance) math, "round", -0.5), "math.round(-0.5) rounds away from zero");
        checkEquals(1.0, callField((LoxInstance) math, "round", 0.5), "math.round(0.5) rounds away from zero");
        checkEquals(5.0, ((LoxCallable) ((LoxInstance) math).fields.get("min"))
                .call(new Object[] {Double.NaN, 5.0}), "math.min ignores a NaN operand");
        check((Double) ((LoxInstance) math).fields.get("pi") > 3.14, "math.pi is present");

        // int()/float() and the integer module (spec/05-stdlib.md).
        checkEquals("Int", call(globals, "type", 1L), "type(Int)");
        checkEquals(5L, call(globals, "int", 5.0), "int(5.0)");
        checkEquals(5L, call(globals, "int", 5L), "int(Int) is itself");
        checkEquals(42L, call(globals, "int", "42"), "int(String)");
        checkThrows(() -> call(globals, "int", 1.5), LoxError.class, "int() rejects a fraction");
        checkThrows(() -> call(globals, "int", "0x10"), LoxError.class, "int() rejects a hex String");
        checkThrows(() -> call(globals, "int", "9223372036854775808"), LoxError.class,
                "int(String) rejects a value above the range");
        checkEquals(3.0, call(globals, "float", 3L), "float(Int)");
        checkEquals(2.5, call(globals, "float", "2.5"), "float(String)");
        checkThrows(() -> call(globals, "float", "1e5"), LoxError.class, "float() rejects an exponent");
        checkThrows(() -> call(globals, "float", "5."), LoxError.class, "float() rejects a trailing point");

        Object integer = globals.get("integer");
        check(integer instanceof LoxInstance, "integer is an instance");
        checkEquals(Long.MAX_VALUE, ((LoxInstance) integer).fields.get("max_value"), "integer.max_value");
        checkEquals(Long.MIN_VALUE, ((LoxInstance) integer).fields.get("min_value"), "integer.min_value");
        checkEquals(Long.MIN_VALUE, callField((LoxInstance) integer, "abs", Long.MIN_VALUE),
                "integer.abs(min) wraps");
        checkEquals(-1L, callField((LoxInstance) integer, "udiv", -1L, 1L), "integer.udiv reads unsigned");
        checkEquals(1L, callField((LoxInstance) integer, "urem", -1L, 2L), "integer.urem reads unsigned");
        check(Boolean.TRUE.equals(callField((LoxInstance) integer, "ult", 1L, -1L)),
                "integer.ult reads unsigned");
        checkEquals("18446744073709551615", callField((LoxInstance) integer, "ustr", -1L),
                "integer.ustr(-1)");
        checkEquals(-1L, callField((LoxInstance) integer, "parse_unsigned", "18446744073709551615"),
                "integer.parse_unsigned(max)");
        checkThrows(() -> callField((LoxInstance) integer, "abs", 1.0), LoxError.class,
                "integer.abs rejects a Number");
        checkThrows(() -> callField((LoxInstance) integer, "udiv", 1L, 0L), LoxError.class,
                "integer.udiv rejects a zero divisor");

        checkOpenRoundtrip(globals);
        checkOsAccess(globals);
        checkIntegerArguments(globals);

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

    // The port-range grid of spec/05-stdlib.md, "Integer arguments": one row
    // per cell of three functions, two kinds of port, and the accepted and
    // refused side of each end of the range. One check per row, so a bound
    // that changes for one kind or one end alone fails.
    private static final Object[][] PORT_GRID = {
        // connect takes a port from 1 through 65535.
        {"connect", 0L, Boolean.FALSE},
        {"connect", 0.0, Boolean.FALSE},
        {"connect", 1L, Boolean.TRUE},
        {"connect", 1.0, Boolean.TRUE},
        {"connect", 65535L, Boolean.TRUE},
        {"connect", 65535.0, Boolean.TRUE},
        {"connect", 65536L, Boolean.FALSE},
        {"connect", 65536.0, Boolean.FALSE},
        // connect_within takes the same port.
        {"connect_within", 0L, Boolean.FALSE},
        {"connect_within", 0.0, Boolean.FALSE},
        {"connect_within", 1L, Boolean.TRUE},
        {"connect_within", 1.0, Boolean.TRUE},
        {"connect_within", 65535L, Boolean.TRUE},
        {"connect_within", 65535.0, Boolean.TRUE},
        {"connect_within", 65536L, Boolean.FALSE},
        {"connect_within", 65536.0, Boolean.FALSE},
        // listen takes a port from 0 through 65535.
        {"listen", -1L, Boolean.FALSE},
        {"listen", -1.0, Boolean.FALSE},
        {"listen", 0L, Boolean.TRUE},
        {"listen", 0.0, Boolean.TRUE},
        {"listen", 65535L, Boolean.TRUE},
        {"listen", 65535.0, Boolean.TRUE},
        {"listen", 65536L, Boolean.FALSE},
        {"listen", 65536.0, Boolean.FALSE},
    };

    // A port that is not an integer at all is refused by every function: a
    // Number that is not integer-valued, and a value of any other kind
    // (spec/05-stdlib.md, "Integer arguments").
    private static final Object[][] NOT_INTEGER_PORTS = {
        {"connect", 1.5, Boolean.FALSE},
        {"connect", Double.NaN, Boolean.FALSE},
        {"connect", Double.POSITIVE_INFINITY, Boolean.FALSE},
        {"connect", Double.NEGATIVE_INFINITY, Boolean.FALSE},
        {"connect", "3", Boolean.FALSE},
        {"connect_within", 1.5, Boolean.FALSE},
        {"connect_within", Double.NaN, Boolean.FALSE},
        {"connect_within", Double.POSITIVE_INFINITY, Boolean.FALSE},
        {"connect_within", Double.NEGATIVE_INFINITY, Boolean.FALSE},
        {"connect_within", "3", Boolean.FALSE},
        {"listen", 1.5, Boolean.FALSE},
        {"listen", Double.NaN, Boolean.FALSE},
        {"listen", Double.POSITIVE_INFINITY, Boolean.FALSE},
        {"listen", Double.NEGATIVE_INFINITY, Boolean.FALSE},
        {"listen", "3", Boolean.FALSE},
    };

    // Mirrors test/test_net_process.cpp's NetProcessTest.InvalidPortFatal for
    // the JVM's own copy of the check. A port outside its range, and a port
    // that is not an integer, is a fatal fault: a catchable one would mean the
    // call reached the connection attempt, whose NetworkError a program's
    // `try` can catch, so the port was not refused at all.
    private static void checkIntegerArguments(LoxGlobals globals) {
        checkGrid(globals, PORT_GRID, 24, "port range");
        checkGrid(globals, NOT_INTEGER_PORTS, 15, "not an integer");
    }

    // Runs one check per row, and one check that the table has `expected` rows
    // with no repeat, so a row that is lost or copied cannot pass.
    private static void checkGrid(LoxGlobals globals, Object[][] grid, int expected,
                                  String noun) {
        check(grid.length == expected, "the " + noun + " grid has one row per cell");
        java.util.Set<String> cells = new java.util.HashSet<>();
        for (Object[] row : grid) {
            cells.add(row[0] + "/" + LoxOps.str(row[1]) + "/"
                      + row[1].getClass().getSimpleName() + "/" + row[2]);
        }
        check(cells.size() == grid.length, "no " + noun + " grid row repeats another");
        for (Object[] row : grid) {
            checkPort(globals, (String) row[0], row[1], (Boolean) row[2]);
        }
    }

    // One cell: an accepted port must reach the network, where a fault is
    // catchable, and a refused one must be a fatal fault.
    private static void checkPort(LoxGlobals globals, String function, Object port,
                                  boolean accepted) {
        String shown = function + "() " + (accepted ? "accepts port " : "rejects port ")
                + LoxOps.str(port) + (port instanceof Long ? "i" : "");
        Object[] args = function.equals("connect_within")
                ? new Object[] {"127.0.0.1", port, 5.0}
                : new Object[] {"127.0.0.1", port};
        if (accepted) {
            checkNotFatal(() -> closeQuietly(call(globals, function, args)), shown);
        } else {
            checkFatal(() -> call(globals, function, args), shown);
        }
    }

    // A Socket or a Server that a call returned, closed, so a check that only
    // wants to see that the call reached the network leaves nothing open.
    private static void closeQuietly(Object value) {
        if (value instanceof LoxSocket) {
            ((LoxSocket) value).close();
        } else if (value instanceof LoxServer) {
            ((LoxServer) value).close();
        }
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
