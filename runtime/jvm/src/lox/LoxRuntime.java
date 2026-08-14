package lox;

import java.io.BufferedOutputStream;
import java.io.BufferedReader;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintStream;

/** Builds a fresh {@link LoxGlobals} populated with the full native stdlib surface. */
public final class LoxRuntime {
    private LoxRuntime() {}

    /**
     * Buffered stdout: PRINT fires millions of times in the self-hosted
     * interpreter, so an unbuffered stream would dominate runtime. A shutdown
     * hook flushes it, so generated code never has to remember to.
     */
    public static final PrintStream out = new PrintStream(
            new BufferedOutputStream(new FileOutputStream(FileDescriptor.out), 1 << 16), false);

    private static final BufferedReader STDIN = new BufferedReader(new InputStreamReader(System.in));

    static {
        Runtime.getRuntime().addShutdownHook(new Thread(out::flush));
    }

    public static LoxGlobals init() {
        LoxGlobals globals = new LoxGlobals();
        registerGlobals(globals);
        registerMath(globals);
        return globals;
    }

    private static void registerGlobals(LoxGlobals globals) {
        // clock()'s epoch is unspecified by the spec (only elapsed time
        // between two calls is meaningful), so a monotonic JVM nanoTime
        // stands in for std::clock()'s process CPU time.
        globals.define("clock", new LoxNative("clock", 0, args -> System.nanoTime() / 1.0e9));
        globals.define("input", new LoxNative("input", 0, args -> {
            try {
                return STDIN.readLine(); // null at EOF becomes Lox nil directly
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
