package lox;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.PrintStream;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.attribute.FileTime;
import java.util.Arrays;

/**
 * Builds a fresh {@link LoxGlobals} populated with the full native stdlib
 * surface.
 *
 * <p><b>Byte boundary rule:</b> a Lox++ string is a byte sequence
 * (spec/03-types.md), not text. Every point where a string crosses into or
 * out of the JVM process — stdout here, stdin here, file I/O in
 * {@link LoxFile} — must read or write it as {@link #CHARSET}
 * (ISO-8859-1: the one Java charset that maps every byte 0-255 to one char
 * and back losslessly). The JVM's default charset is UTF-8 in the managed
 * image, which re-encodes any byte 0x80-0xFF into two bytes and turns an
 * unpaired high byte from stdin into U+FFFD.
 * The bytecode decoder that reads string constants out of the chunk must
 * decode them with this same charset, or {@code len()} and {@code s[i]}
 * will drift from the native VM on any non-ASCII byte.
 */
public final class LoxRuntime {
    private LoxRuntime() {}

    public static final Charset CHARSET = StandardCharsets.ISO_8859_1;

    /**
     * Buffered stdout: PRINT fires millions of times in the self-hosted
     * interpreter, so an unbuffered stream would dominate runtime. A shutdown
     * hook flushes it, so generated code never has to remember to.
     */
    public static final PrintStream out = new PrintStream(
            new BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 1 << 16), false, CHARSET);

    private static final InputStream STDIN = new BufferedInputStream(System.in);

    static {
        Runtime.getRuntime().addShutdownHook(new Thread(out::flush));
    }

    // The one LoxGlobals instance for this JVM process (design decision A2:
    // one dynamic map, not one static field per global name — this is a
    // different concern, "where does a generated method find the map").
    // main's own frame holds a reference in a JVM local, but a generated
    // LoxFn$<n>.invoke has no such local, so it reads this static instead
    // of threading the reference through every call. Safe
    // because exactly one Lox++ program ever runs per JVM process here.
    private static LoxGlobals current;

    // Command-line arguments after the program name, forwarded by the
    // generated script's main() before the first script statement runs. An
    // empty array when main received none, as in the differential harness.
    private static String[] programArgs = new String[0];

    /** The generated script's main() forwards its own argv here before the script body runs. */
    public static void setProgramArgs(String[] args) {
        programArgs = args;
    }

    /**
     * Reads one line as raw bytes (0-255), never decoding them as text.
     * Returns nil at immediate EOF, matching std::getline's failure rule
     * (globals.cpp): a partial trailing line with no terminator still
     * counts as a line, and only a read that captures zero bytes is nil.
     * Package-visible so a test can drive it without real stdin.
     */
    static String readByteLine(InputStream in) throws IOException {
        StringBuilder line = new StringBuilder();
        boolean sawByte = false;
        int b;
        while ((b = in.read()) != -1) {
            sawByte = true;
            if (b == '\n') {
                break;
            }
            line.append((char) (b & 0xFF));
        }
        return sawByte ? line.toString() : null;
    }

    public static LoxGlobals init() {
        LoxGlobals globals = new LoxGlobals();
        registerGlobals(globals);
        registerMath(globals);
        registerReflection(globals);
        current = globals;
        return globals;
    }

    /** The instance the script's own {@link #init} call built. Null before that call runs. */
    public static LoxGlobals current() {
        return current;
    }

    private static void registerGlobals(LoxGlobals globals) {
        // clock()'s epoch is unspecified by the spec (only elapsed time
        // between two calls is meaningful), so a monotonic JVM nanoTime
        // stands in for std::clock()'s process CPU time.
        globals.define("clock", new LoxNative("clock", 0, args -> System.nanoTime() / 1.0e9));
        globals.define("input", new LoxNative("input", 0, args -> {
            try {
                return readByteLine(STDIN); // null at EOF becomes Lox nil directly
            } catch (IOException e) {
                return null;
            }
        }));
        globals.define("str", new LoxNative("str", 1, args -> LoxOps.stringify(args[0])));
        globals.define("len", new LoxNative("len", 1, args -> {
            Object v = args[0];
            if (v instanceof LoxList) {
                return (double) ((LoxList) v).elements.size();
            }
            if (v instanceof String) {
                return (double) ((String) v).length();
            }
            if (v instanceof LoxMap) {
                return (double) ((LoxMap) v).size();
            }
            throw new LoxError("len() argument must be a list, string, or map.");
        }));
        globals.define("open", new LoxNative("open", 2, args -> {
            if (!(args[0] instanceof String) || !(args[1] instanceof String)) {
                throw new LoxError("open() requires string path and mode.");
            }
            return LoxFile.open((String) args[0], (String) args[1]);
        }));
        registerOsAccess(globals);
    }

    // Mirrors src/stdlib/os_api.cpp: args, env, exit, time, sleep, and the
    // file-system predicates exists()/is_dir()/is_file()/stat(). Kept in the
    // JVM runtime so a program using them does not DIVERGE in the differential
    // suite (tools/diff_runtimes.py) against the native VM.
    private static void registerOsAccess(LoxGlobals globals) {
        globals.define("args", new LoxNative("args", 0, unused -> {
            LoxList list = new LoxList();
            for (String s : programArgs) {
                list.elements.add(s);
            }
            return list;
        }));
        globals.define("env", new LoxNative("env", 1, args -> {
            if (!(args[0] instanceof String)) {
                throw new LoxError("Expected a string argument.");
            }
            return System.getenv((String) args[0]); // null -> Lox nil, matching getenv(3)
        }));
        globals.define("exit", new LoxNative("exit", 1, args -> {
            if (!(args[0] instanceof Double)) {
                throw new LoxError("exit() code must be a number.");
            }
            double raw = (Double) args[0];
            // Truncate toward zero, the C-style integral conversion (os_api.cpp).
            // Reject a value Java's cast cannot represent, mirroring the
            // native VM's finite/int-range guard.
            if (Double.isNaN(raw) || Double.isInfinite(raw)
                    || raw > Integer.MAX_VALUE || raw < Integer.MIN_VALUE) {
                throw new LoxError(
                        "exit() code must be a finite number in the integer range.");
            }
            System.exit((int) raw); // range-guarded above, so the cast truncates toward zero, matching C
            throw new AssertionError("System.exit must not return");
        }));
        globals.define("time", new LoxNative("time", 0, unused -> System.currentTimeMillis() / 1000.0));
        globals.define("sleep", new LoxNative("sleep", 1, args -> {
            if (!(args[0] instanceof Double)) {
                throw new LoxError("sleep() duration must be a number.");
            }
            double seconds = (Double) args[0];
            if (seconds > 0) {
                try {
                    Thread.sleep((long) (seconds * 1000.0));
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
            return null;
        }));
        globals.define("exists", new LoxNative("exists", 1, args -> {
            if (!(args[0] instanceof String)) {
                throw new LoxError("Expected a string argument.");
            }
            return Files.exists(Paths.get((String) args[0]));
        }));
        globals.define("is_dir", new LoxNative("is_dir", 1, args -> {
            if (!(args[0] instanceof String)) {
                throw new LoxError("Expected a string argument.");
            }
            return Files.isDirectory(Paths.get((String) args[0]));
        }));
        globals.define("is_file", new LoxNative("is_file", 1, args -> {
            if (!(args[0] instanceof String)) {
                throw new LoxError("Expected a string argument.");
            }
            return Files.isRegularFile(Paths.get((String) args[0]));
        }));
        globals.define("stat", new LoxNative("stat", 1, args -> {
            if (!(args[0] instanceof String)) {
                throw new LoxError("Expected a string argument.");
            }
            Path p = Paths.get((String) args[0]);
            if (!Files.exists(p)) {
                return null; // Lox nil, matching os_api.cpp's statNative
            }
            LoxMap map = new LoxMap();
            map.put("exists", true);
            map.put("is_dir", Files.isDirectory(p));
            map.put("is_file", Files.isRegularFile(p));
            if (Files.isRegularFile(p)) {
                try {
                    map.put("size", (double) Files.size(p));
                } catch (IOException e) {
                    // size key omitted, matching statNative when file_size errs
                }
            }
            try {
                FileTime mtime = Files.getLastModifiedTime(p);
                map.put("mtime", (double) (mtime.toMillis() / 1000.0));
            } catch (IOException e) {
                // mtime key omitted, matching statNative when last_write_time errs
            }
            return map;
        }));
    }

    // `math` is a plain instance with native-function fields (not methods),
    // exactly like ObjInstance in math_module.cpp — GET_PROPERTY/INVOKE
    // already handle a callable field with no extra machinery.
    private static void registerMath(LoxGlobals globals) {
        LoxInstance math = new LoxInstance(new LoxClass("Math", null));
        math.fields.put("abs", mathUnary("abs", Math::abs));
        math.fields.put("ceil", mathUnary("ceil", Math::ceil));
        math.fields.put("floor", mathUnary("floor", Math::floor));
        math.fields.put("round", mathUnary("round", LoxRuntime::roundHalfAwayFromZero));
        math.fields.put("sqrt", mathUnary("sqrt", Math::sqrt));
        math.fields.put("cbrt", mathUnary("cbrt", Math::cbrt));
        math.fields.put("exp", mathUnary("exp", Math::exp));
        math.fields.put("log", mathUnary("log", Math::log));
        math.fields.put("log2", mathUnary("log2", x -> Math.log(x) / Math.log(2)));
        math.fields.put("log10", mathUnary("log10", Math::log10));
        math.fields.put("sin", mathUnary("sin", Math::sin));
        math.fields.put("cos", mathUnary("cos", Math::cos));
        math.fields.put("tan", mathUnary("tan", Math::tan));
        math.fields.put("asin", mathUnary("asin", Math::asin));
        math.fields.put("acos", mathUnary("acos", Math::acos));
        math.fields.put("atan", mathUnary("atan", Math::atan));
        math.fields.put("pow", mathBinary("pow", Math::pow));
        math.fields.put("atan2", mathBinary("atan2", Math::atan2));
        math.fields.put("hypot", mathBinary("hypot", Math::hypot));
        math.fields.put("min", mathBinary("min", LoxRuntime::fmin));
        math.fields.put("max", mathBinary("max", LoxRuntime::fmax));
        math.fields.put("pi", Math.PI);
        math.fields.put("e", Math.E);
        math.fields.put("inf", Double.POSITIVE_INFINITY);
        math.fields.put("nan", Double.NaN);
        globals.define("math", math);
    }

    // Mirrors src/stdlib/reflect_api.cpp: type/fields/methods/getField/
    // setField/hasField/callMethod, the introspection surface over
    // LoxInstance's field table and LoxClass's method table. `callMethod` is
    // capped to natives-only on every backend, including this one, even
    // though a JVM closure call needs no re-entrant interpreter loop — see
    // notes/expressiveness-roadmap.md item 1: lifting this only on JVM would
    // let JVM print real output where native raises an error, which
    // tools/diff_runtimes.py would catch as a divergence.
    private static void registerReflection(LoxGlobals globals) {
        globals.define("type", new LoxNative("type", 1, args -> typeNameOf(args[0])));
        globals.define("fields", new LoxNative("fields", 1, args -> {
            LoxInstance inst = requireInstance(args[0], "Expected an instance.");
            LoxList list = new LoxList();
            list.elements.addAll(inst.fields.keySet());
            return list;
        }));
        globals.define("methods", new LoxNative("methods", 1, args -> {
            if (!(args[0] instanceof LoxClass)) {
                throw new LoxError("Expected a class.");
            }
            LoxList list = new LoxList();
            list.elements.addAll(((LoxClass) args[0]).methods.keySet());
            return list;
        }));
        globals.define("getField", new LoxNative("getField", 2, args -> {
            LoxInstance inst = requireInstance(args[0], "Only instances have properties.");
            String name = requireFieldName(args[1]);
            return inst.fields.get(name); // absent field and a stored nil both read back as null
        }));
        globals.define("hasField", new LoxNative("hasField", 2, args -> {
            LoxInstance inst = requireInstance(args[0], "Only instances have properties.");
            String name = requireFieldName(args[1]);
            return inst.fields.containsKey(name);
        }));
        globals.define("setField", new LoxNative("setField", 3, args -> {
            LoxInstance inst = requireInstance(args[0], "Only instances have fields.");
            String name = requireFieldName(args[1]);
            inst.fields.put(name, args[2]);
            return args[2]; // assignment is an expression, per Property Set semantics
        }));
        globals.define("callMethod", new LoxNative("callMethod", -1, LoxRuntime::callMethod));
    }

    // type(x)'s ladder groups values the same way LoxOps.stringify does:
    // a closure and a plain native are both "Function"; a user-defined bound
    // method and a bound Map/File native are both "BoundMethod" (see
    // LoxNative.receiver for how the two natives are told apart). This is a
    // language-level grouping (spec/03-types.md has one Function heading and
    // one BoundMethod heading), not a Java class per Lox type.
    private static String typeNameOf(Object v) {
        if (v == null) {
            return "Nil";
        }
        if (v instanceof Boolean) {
            return "Boolean";
        }
        if (v instanceof Double) {
            return "Number";
        }
        if (v instanceof String) {
            return "String";
        }
        if (v instanceof LoxClosure) {
            return "Function";
        }
        if (v instanceof LoxNative) {
            return (((LoxNative) v).receiver != null) ? "BoundMethod" : "Function";
        }
        if (v instanceof LoxBoundMethod) {
            return "BoundMethod";
        }
        if (v instanceof LoxClass) {
            return "Class";
        }
        if (v instanceof LoxInstance) {
            return "Instance";
        }
        if (v instanceof LoxList) {
            return "List";
        }
        if (v instanceof LoxMap) {
            return "Map";
        }
        if (v instanceof LoxFile) {
            return "File";
        }
        if (v instanceof LoxIterator) {
            return "Iterator";
        }
        if (v instanceof LoxEnumCtor) {
            return "EnumConstructor";
        }
        if (v instanceof LoxEnum) {
            return "Enum";
        }
        throw new IllegalStateException("type(): unrecognized value " + v.getClass());
    }

    private static LoxInstance requireInstance(Object v, String message) {
        if (!(v instanceof LoxInstance)) {
            throw new LoxError(message);
        }
        return (LoxInstance) v;
    }

    private static String requireFieldName(Object v) {
        if (!(v instanceof String)) {
            throw new LoxError("Field name must be a string.");
        }
        return (String) v;
    }

    // callMethod(inst, name, ...args) resolves exactly like LoxOps.invoke's
    // instance branch (fields shadow methods), but only ever calls a native:
    // a resolved LoxClosure/LoxBoundMethod is a deliberate v1 runtime error,
    // matching native's restriction (see this method's caller for why). A
    // resolved LoxNative — whether a plain global native stored in a field,
    // or a bound Map/File native such as `someMap.has` stored in a field —
    // already captures whatever receiver it needs as a Java closure, so,
    // unlike native's ObjBoundNative, no receiver substitution is needed
    // here before forwarding the trailing args.
    private static Object callMethod(Object[] args) {
        if (args.length < 2) {
            throw new LoxError("Expected at least 2 arguments.");
        }
        LoxInstance inst = requireInstance(args[0], "Only instances have methods.");
        String name = requireFieldName(args[1]);

        Object callee;
        if (inst.fields.containsKey(name)) {
            callee = inst.fields.get(name);
        } else {
            LoxClosure method = inst.klass.findMethod(name);
            if (method == null) {
                throw new LoxError("Undefined property '" + name + "'.");
            }
            callee = method;
        }

        Object[] forwarded = Arrays.copyOfRange(args, 2, args.length);
        if (callee instanceof LoxNative) {
            return ((LoxNative) callee).call(forwarded);
        }
        if (callee instanceof LoxClosure || callee instanceof LoxBoundMethod) {
            throw new LoxError("callMethod does not support user-defined methods yet.");
        }
        throw new LoxError("Can only call functions, classes and enums.");
    }

    private interface DoubleFn {
        double apply(double x);
    }

    private interface DoubleFn2 {
        double apply(double x, double y);
    }

    private static LoxNative mathUnary(String name, DoubleFn f) {
        return new LoxNative(name, 1, args -> {
            if (!(args[0] instanceof Double)) {
                throw new LoxError("math function argument must be a number.");
            }
            return f.apply((Double) args[0]);
        });
    }

    private static LoxNative mathBinary(String name, DoubleFn2 f) {
        return new LoxNative(name, 2, args -> {
            if (!(args[0] instanceof Double) || !(args[1] instanceof Double)) {
                throw new LoxError("math function arguments must be numbers.");
            }
            return f.apply((Double) args[0], (Double) args[1]);
        });
    }

    // std::round rounds half away from zero; Math.round rounds half toward
    // +infinity (Math.round(-0.5) == 0, but C's round(-0.5) == -1.0).
    private static double roundHalfAwayFromZero(double x) {
        return Math.copySign(Math.floor(Math.abs(x) + 0.5), x);
    }

    // std::fmin/fmax ignore a NaN operand when the other is a number;
    // Math.min/max instead propagate NaN.
    private static double fmin(double a, double b) {
        if (Double.isNaN(a)) {
            return b;
        }
        if (Double.isNaN(b)) {
            return a;
        }
        return Math.min(a, b);
    }

    private static double fmax(double a, double b) {
        if (Double.isNaN(a)) {
            return b;
        }
        if (Double.isNaN(b)) {
            return a;
        }
        return Math.max(a, b);
    }
}
