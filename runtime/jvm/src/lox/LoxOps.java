package lox;

import java.math.BigDecimal;
import java.math.MathContext;
import java.math.RoundingMode;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Static helpers for every opcode that is not pure stack motion. Semantics
 * come from src/vm.cpp; error text is copied where practical, but the
 * differential gate (N11) compares stdout only, so wording is not
 * load-bearing — only the value each helper produces is.
 */
public final class LoxOps {
    private LoxOps() {}

    // ------------------------------------------------------------------
    // Truthiness (operator! in value.h: numbers and objects are truthy)
    // ------------------------------------------------------------------

    public static boolean isFalsy(Object v) {
        if (v instanceof Boolean) {
            return !(Boolean) v;
        }
        return v == null;
    }

    public static Object not(Object v) {
        return isFalsy(v);
    }

    // ------------------------------------------------------------------
    // Arithmetic
    // ------------------------------------------------------------------

    public static double checkNumber(Object v) {
        if (!(v instanceof Double)) {
            throw new LoxError("Operand must be a number.");
        }
        return (Double) v;
    }

    private static void checkNumbers(Object a, Object b) {
        if (!(a instanceof Double) || !(b instanceof Double)) {
            throw new LoxError("Operands must be numbers.");
        }
    }

    public static Object add(Object a, Object b) {
        if (a instanceof String && b instanceof String) {
            return (String) a + (String) b;
        }
        checkNumbers(a, b);
        return (Double) a + (Double) b;
    }

    public static Object subtract(Object a, Object b) {
        checkNumbers(a, b);
        return (Double) a - (Double) b;
    }

    public static Object multiply(Object a, Object b) {
        checkNumbers(a, b);
        return (Double) a * (Double) b;
    }

    public static Object divide(Object a, Object b) {
        checkNumbers(a, b);
        return (Double) a / (Double) b;
    }

    /** Floor-division sign: the result takes the sign of `b` (Python/Lua rule), matching vm.cpp exactly. */
    public static Object modulo(Object a, Object b) {
        checkNumbers(a, b);
        double bd = (Double) b;
        double result = (Double) a % bd; // Java's `%` on doubles is fmod, per JLS 15.17.3
        if (result != 0 && (result < 0) != (bd < 0)) {
            result += bd;
        }
        return result;
    }

    public static Object negate(Object a) {
        return -checkNumber(a);
    }

    // ------------------------------------------------------------------
    // Comparisons
    // ------------------------------------------------------------------

    /**
     * Strings compare by content, not identity: the native VM relies on
     * interning (equal content -> same ObjString pointer) to fold string
     * equality into its generic Obj* identity check. This runtime does not
     * intern computed strings (ADD produces a fresh java.lang.String each
     * time), so string equality is special-cased here instead.
     */
    public static boolean equal(Object a, Object b) {
        if (a instanceof Double && b instanceof Double) {
            return ((Double) a).doubleValue() == ((Double) b).doubleValue();
        }
        if (a instanceof Double || b instanceof Double) {
            return false;
        }
        if (a instanceof Boolean && b instanceof Boolean) {
            return a.equals(b);
        }
        if (a instanceof String && b instanceof String) {
            return a.equals(b);
        }
        return a == b; // nil (null == null) and every identity-equality object type
    }

    public static boolean greater(Object a, Object b) {
        checkNumbers(a, b);
        return (Double) a > (Double) b;
    }

    public static boolean less(Object a, Object b) {
        checkNumbers(a, b);
        return (Double) a < (Double) b;
    }

    // ------------------------------------------------------------------
    // Sequences: in / slice / index
    // ------------------------------------------------------------------

    /** Public for codegen (N6's BUILD_MAP): every key must pass this before the map is built, per vm.cpp. */
    public static void checkMapKey(Object key) {
        if (key == null || key instanceof Boolean || key instanceof String) {
            return;
        }
        if (key instanceof Double && !Double.isNaN((Double) key)) {
            return;
        }
        throw new LoxError(
                "Map keys must be Bool, Number, Nil, or String. NaN is not allowed.");
    }

    /** Operand order matches the on-stack order (elem below seq) — chunk.h's IN pops [elem, seq]. */
    public static boolean in(Object elem, Object seq) {
        if (seq instanceof LoxList) {
            for (Object v : ((LoxList) seq).elements) {
                if (equal(v, elem)) {
                    return true;
                }
            }
            return false;
        }
        if (seq instanceof String) {
            if (!(elem instanceof String)) {
                throw new LoxError("Left operand of 'in' on a string must be a string.");
            }
            return ((String) seq).contains((String) elem);
        }
        if (seq instanceof LoxMap) {
            checkMapKey(elem);
            return ((LoxMap) seq).has(elem);
        }
        throw new LoxError("Right operand of 'in' must be a list, string, or map.");
    }

    private static int sliceIndex(Object v) {
        if (!(v instanceof Double)) {
            throw new LoxError("Slice index must be a number.");
        }
        double d = (Double) v;
        if (d != Math.floor(d)) {
            throw new LoxError("Slice index must be an integer.");
        }
        if (d < 0.0) {
            throw new LoxError("Slice index must be non-negative.");
        }
        return (int) d;
    }

    /** [seq, start, end] — the same parameter order the operand stack holds bottom-up. */
    public static Object slice(Object seq, Object startVal, Object endVal) {
        if (!(seq instanceof LoxList) && !(seq instanceof String)) {
            throw new LoxError("Slice requires a List or String.");
        }
        int start = sliceIndex(startVal);
        int end = sliceIndex(endVal);
        if (seq instanceof LoxList) {
            List<Object> src = ((LoxList) seq).elements;
            int n = src.size();
            int s = Math.min(start, n);
            int e = Math.min(end, n);
            LoxList result = new LoxList();
            if (s < e) {
                result.elements.addAll(src.subList(s, e));
            }
            return result;
        }
        String str = (String) seq;
        int n = str.length();
        int s = Math.min(start, n);
        int e = Math.min(end, n);
        return (s < e) ? str.substring(s, e) : "";
    }

    private static int boundedIndex(Object indexVal, int size, String kind) {
        if (!(indexVal instanceof Double)) {
            throw new LoxError(kind + " index must be a number.");
        }
        double d = (Double) indexVal;
        if (d != Math.floor(d)) {
            throw new LoxError(kind + " index must be an integer.");
        }
        int idx = (int) d;
        if (idx < 0 || idx >= size) {
            throw new LoxError(kind + " index out of bounds.");
        }
        return idx;
    }

    public static Object getIndex(Object collection, Object index) {
        if (collection instanceof LoxList) {
            List<Object> elements = ((LoxList) collection).elements;
            return elements.get(boundedIndex(index, elements.size(), "List"));
        }
        if (collection instanceof String) {
            String s = (String) collection;
            return String.valueOf(s.charAt(boundedIndex(index, s.length(), "String")));
        }
        if (collection instanceof LoxMap) {
            checkMapKey(index);
            return ((LoxMap) collection).get(index); // absent key and a stored nil both read back as null
        }
        if (collection instanceof LoxEnum) {
            if (!(index instanceof Double)) {
                throw new LoxError("Enum field index must be a number.");
            }
            Object[] payload = ((LoxEnum) collection).payload;
            int idx = (int) (double) (Double) index;
            if (idx < 0 || idx >= payload.length) {
                throw new LoxError("Enum field index " + idx + " out of range.");
            }
            return payload[idx];
        }
        throw new LoxError("Only lists, strings, and maps can be indexed.");
    }

    public static Object setIndex(Object collection, Object index, Object value) {
        if (collection instanceof String) {
            throw new LoxError(
                    "Strings are immutable and cannot be indexed for assignment.");
        }
        if (collection instanceof LoxMap) {
            checkMapKey(index);
            ((LoxMap) collection).put(index, value);
            return value;
        }
        if (!(collection instanceof LoxList)) {
            throw new LoxError("Only lists and maps can be indexed for assignment.");
        }
        List<Object> elements = ((LoxList) collection).elements;
        elements.set(boundedIndex(index, elements.size(), "List"), value);
        return value; // assignment is an expression
    }

    public static LoxIterator getIter(Object iterable) {
        if (!(iterable instanceof LoxList) && !(iterable instanceof String)
                && !(iterable instanceof LoxMap)) {
            throw new LoxError(
                    "Value is not iterable (expected list, string, or map).");
        }
        return new LoxIterator(iterable);
    }

    /** Matches Op::IS_SEQ exactly: List and String only — Map is not included (vm.cpp). */
    public static boolean isSeq(Object v) {
        return v instanceof LoxList || v instanceof String;
    }

    public static void matchError() {
        throw new LoxError("MatchError: no matching arm.");
    }

    // ------------------------------------------------------------------
    // instanceof / properties / methods
    // ------------------------------------------------------------------

    /** Never throws: an undefined or non-class name simply fails to match, as in vm.cpp's INSTANCEOF. */
    public static boolean instanceOf(Object val, LoxGlobals globals, String className) {
        if (!(val instanceof LoxInstance) || !globals.isDefined(className)) {
            return false;
        }
        Object classVal = globals.get(className);
        if (!(classVal instanceof LoxClass)) {
            return false;
        }
        LoxClass target = (LoxClass) classVal;
        LoxClass k = ((LoxInstance) val).klass;
        while (k != null) {
            if (k == target) {
                return true;
            }
            k = k.superclass;
        }
        return false;
    }

    public static Object getProperty(Object obj, String name) {
        if (obj instanceof LoxFile) {
            LoxCallable m = ((LoxFile) obj).getMethod(name);
            if (m == null) {
                throw new LoxError("Undefined property '" + name + "' on file.");
            }
            return m;
        }
        if (obj instanceof LoxMap) {
            LoxCallable m = ((LoxMap) obj).getMethod(name);
            if (m == null) {
                throw new LoxError("Undefined property '" + name + "' on map.");
            }
            return m;
        }
        if (!(obj instanceof LoxInstance)) {
            throw new LoxError("Only instances have properties.");
        }
        LoxInstance instance = (LoxInstance) obj;
        if (instance.fields.containsKey(name)) {
            return instance.fields.get(name);
        }
        LoxClosure method = instance.klass.findMethod(name);
        if (method == null) {
            throw new LoxError("Undefined property '" + name + "'.");
        }
        return new LoxBoundMethod(instance, method);
    }

    public static Object setProperty(Object obj, String name, Object value) {
        if (!(obj instanceof LoxInstance)) {
            throw new LoxError("Only instances have fields.");
        }
        ((LoxInstance) obj).fields.put(name, value);
        return value; // assignment is an expression
    }

    public static void defineMethod(LoxClass klass, String name, LoxClosure method) {
        klass.methods.put(name, method);
    }

    /**
     * The INHERIT opcode's guard. Codegen would otherwise need a raw
     * {@code checkcast} to LoxClass, which throws ClassCastException instead
     * of LoxError (PR #97 review finding R5). Call this before constructing
     * the subclass, so its superclass is already validated.
     */
    public static LoxClass inherit(Object superclassVal) {
        if (!(superclassVal instanceof LoxClass)) {
            throw new LoxError("Superclass must be a class.");
        }
        return (LoxClass) superclassVal;
    }

    /** The GET_TAG opcode's guard — same ClassCastException problem as {@link #inherit}. */
    public static double getTag(Object v) {
        if (!(v instanceof LoxEnum)) {
            throw new LoxError("GET_TAG: expected an enum value.");
        }
        return ((LoxEnum) v).ctor.tag;
    }

    /** The INVOKE fast path: dispatches on the receiver's runtime kind (P6), not on one static type. */
    public static Object invoke(Object receiver, String name, Object[] args) {
        if (receiver instanceof LoxInstance) {
            LoxInstance instance = (LoxInstance) receiver;
            if (instance.fields.containsKey(name)) {
                Object fieldVal = instance.fields.get(name);
                // vm.cpp lines 518-533 call only a closure or a native field
                // this way; a class, an enum constructor, or a bound method
                // is a runtime error here, even though all four implement
                // LoxCallable (PR #97 review finding R4).
                if (fieldVal instanceof LoxClosure) {
                    return ((LoxClosure) fieldVal).call(args);
                }
                if (fieldVal instanceof LoxNative) {
                    return ((LoxNative) fieldVal).call(args);
                }
                throw new LoxError("Can only call functions, classes and enums.");
            }
            LoxClosure method = instance.klass.findMethod(name);
            if (method == null) {
                throw new LoxError("Undefined property '" + name + "'.");
            }
            return method.callAsSelf(instance, args);
        }
        if (receiver instanceof LoxList) {
            return invokeListMethod((LoxList) receiver, name, args);
        }
        if (receiver instanceof LoxFile) {
            return invokeFileMethod((LoxFile) receiver, name, args);
        }
        if (receiver instanceof LoxMap) {
            return invokeMapMethod((LoxMap) receiver, name, args);
        }
        throw new LoxError("Only instances, files, and maps have methods.");
    }

    private static Object invokeListMethod(LoxList list, String name, Object[] args) {
        switch (name) {
        case "append":
            if (args.length != 1) {
                throw new LoxError("'append' expects 1 argument but got " + args.length + ".");
            }
            list.elements.add(args[0]);
            return null;
        case "pop":
            if (args.length != 0) {
                throw new LoxError("'pop' expects 0 arguments but got " + args.length + ".");
            }
            if (list.elements.isEmpty()) {
                throw new LoxError("Cannot pop from an empty list.");
            }
            return list.elements.remove(list.elements.size() - 1);
        case "remove":
            if (args.length != 1) {
                throw new LoxError("'remove' expects 1 argument but got " + args.length + ".");
            }
            for (int i = 0; i < list.elements.size(); i++) {
                if (equal(list.elements.get(i), args[0])) {
                    list.elements.remove(i);
                    return null;
                }
            }
            throw new LoxError("Value not found in list.");
        default:
            throw new LoxError("Undefined method '" + name + "' on list.");
        }
    }

    /**
     * Dispatches by name with no LoxNative allocation — the INVOKE fast path
     * matches vm.cpp's own fast path, which calls the native C function
     * directly and never builds an intermediate ObjNative per call (PR #97
     * review finding R3). {@link LoxMap#getMethod} still allocates (once,
     * cached) for the separate GET_PROPERTY case.
     */
    private static Object invokeMapMethod(LoxMap map, String name, Object[] args) {
        switch (name) {
        case "has":
            requireArity(args, 1, "has");
            checkMapKey(args[0]);
            return map.has(args[0]);
        case "del":
            requireArity(args, 1, "del");
            checkMapKey(args[0]);
            map.remove(args[0]);
            return null;
        case "keys": {
            requireArity(args, 0, "keys");
            LoxList list = new LoxList();
            for (Map.Entry<Object, Object> e : map.entrySet()) {
                list.elements.add(e.getKey());
            }
            return list;
        }
        case "values": {
            requireArity(args, 0, "values");
            LoxList list = new LoxList();
            for (Map.Entry<Object, Object> e : map.entrySet()) {
                list.elements.add(e.getValue());
            }
            return list;
        }
        case "entries": {
            requireArity(args, 0, "entries");
            LoxList list = new LoxList();
            for (Map.Entry<Object, Object> e : map.entrySet()) {
                LoxList pair = new LoxList();
                pair.elements.add(e.getKey());
                pair.elements.add(e.getValue());
                list.elements.add(pair);
            }
            return list;
        }
        default:
            throw new LoxError("Undefined method '" + name + "' on map.");
        }
    }

    /** Same no-allocation dispatch as {@link #invokeMapMethod}, for the file API. */
    private static Object invokeFileMethod(LoxFile file, String name, Object[] args) {
        switch (name) {
        case "read":
            requireArity(args, 0, "read");
            return file.read();
        case "readline":
            requireArity(args, 0, "readline");
            return file.readline();
        case "readlines":
            requireArity(args, 0, "readlines");
            return file.readlines();
        case "write":
            requireArity(args, 1, "write");
            file.write(checkStringArg(args[0], "write"));
            return null;
        case "writeline":
            requireArity(args, 1, "writeline");
            file.writeline(checkStringArg(args[0], "writeline"));
            return null;
        case "close":
            requireArity(args, 0, "close");
            file.close();
            return null;
        default:
            throw new LoxError("Undefined method '" + name + "' on file.");
        }
    }

    private static void requireArity(Object[] args, int arity, String method) {
        if (args.length != arity) {
            throw new LoxError(
                    "'" + method + "' expects " + arity + " argument(s) but got " + args.length + ".");
        }
    }

    private static String checkStringArg(Object v, String method) {
        if (!(v instanceof String)) {
            throw new LoxError("'" + method + "' argument must be a string.");
        }
        return (String) v;
    }

    public static Object getSuper(Object superclassVal, String name, Object self) {
        LoxClosure method = ((LoxClass) superclassVal).findMethod(name);
        if (method == null) {
            throw new LoxError("Undefined property '" + name + "'.");
        }
        return new LoxBoundMethod(self, method);
    }

    public static Object superInvoke(Object superclassVal, String name, Object self, Object[] args) {
        LoxClosure method = ((LoxClass) superclassVal).findMethod(name);
        if (method == null) {
            throw new LoxError("Undefined property '" + name + "'.");
        }
        return method.callAsSelf(self, args);
    }

    // ------------------------------------------------------------------
    // print / stringify
    // ------------------------------------------------------------------

    public static void print(Object v) {
        LoxRuntime.out.print(stringify(v));
        LoxRuntime.out.print('\n');
    }

    public static String stringify(Object v) {
        if (v == null) {
            return "nil";
        }
        if (v instanceof Boolean) {
            return ((Boolean) v) ? "true" : "false";
        }
        if (v instanceof Double) {
            return formatNumber((Double) v);
        }
        if (v instanceof String) {
            return (String) v;
        }
        if (v instanceof LoxClosure) {
            String name = ((LoxClosure) v).name;
            return (name == null) ? "<script>" : "<fn " + name + ">";
        }
        if (v instanceof LoxNative) {
            return "<native fn>";
        }
        if (v instanceof LoxBoundMethod) {
            return "<fn " + ((LoxBoundMethod) v).method.name + ">";
        }
        if (v instanceof LoxClass) {
            return ((LoxClass) v).name;
        }
        if (v instanceof LoxInstance) {
            return ((LoxInstance) v).klass.name + " instance";
        }
        if (v instanceof LoxFile) {
            return "<file>";
        }
        if (v instanceof LoxIterator) {
            return "<iterator>";
        }
        if (v instanceof LoxList) {
            StringBuilder sb = new StringBuilder("[");
            List<Object> elements = ((LoxList) v).elements;
            for (int i = 0; i < elements.size(); i++) {
                if (i > 0) {
                    sb.append(", ");
                }
                sb.append(stringify(elements.get(i)));
            }
            return sb.append(']').toString();
        }
        if (v instanceof LoxMap) {
            StringBuilder sb = new StringBuilder("{");
            boolean first = true;
            for (Map.Entry<Object, Object> e : ((LoxMap) v).entrySet()) {
                if (!first) {
                    sb.append(", ");
                }
                first = false;
                sb.append(stringify(e.getKey())).append(": ").append(stringify(e.getValue()));
            }
            return sb.append('}').toString();
        }
        if (v instanceof LoxEnumCtor) {
            LoxEnumCtor c = (LoxEnumCtor) v;
            return "<ctor " + c.enumName + "::" + c.ctorName + ">";
        }
        if (v instanceof LoxEnum) {
            LoxEnum e = (LoxEnum) v;
            StringBuilder sb = new StringBuilder(e.ctor.enumName).append("::").append(e.ctor.ctorName);
            if (e.payload.length > 0) {
                sb.append('(');
                for (int i = 0; i < e.payload.length; i++) {
                    if (i > 0) {
                        sb.append(", ");
                    }
                    sb.append(stringify(e.payload[i]));
                }
                sb.append(')');
            }
            return sb.toString();
        }
        throw new IllegalStateException("stringify: unrecognized value type " + v.getClass());
    }

    /**
     * Reproduces C's {@code snprintf("%g", d)} byte for byte (6 significant
     * digits, trailing zeros stripped, {@code e±0N} exponents) — see
     * spec/03-types.md's Number row and value.cpp's stringify. Java's
     * Double.toString uses a different algorithm entirely (e.g. it never
     * strips to a bare integer, and switches to scientific notation on a
     * different threshold), so it cannot be used here.
     */
    static String formatNumber(double d) {
        if (Double.isNaN(d)) {
            // glibc's printf prints the sign bit of the NaN payload itself;
            // an indeterminate 0.0/0.0 on x86_64 carries a set sign bit
            // (confirmed against build/loxpp, matching this JVM's runtime
            // division on the same hardware), hence "-nan" rather than "nan".
            return (Double.doubleToRawLongBits(d) < 0) ? "-nan" : "nan";
        }
        if (Double.isInfinite(d)) {
            return (d > 0) ? "inf" : "-inf";
        }
        boolean negative = Double.doubleToRawLongBits(d) < 0;
        double mag = Math.abs(d);
        if (mag == 0.0) {
            return negative ? "-0" : "0";
        }

        // Round to 6 significant digits from the double's exact binary value —
        // the same correctly-rounded conversion snprintf performs, not a
        // round-trip through the shortest decimal string.
        BigDecimal rounded = new BigDecimal(mag).round(new MathContext(6, RoundingMode.HALF_EVEN));
        int exponent = rounded.precision() - rounded.scale() - 1; // %g's X
        String digits = rounded.unscaledValue().toString();

        String body;
        if (exponent < -4 || exponent >= 6) {
            StringBuilder mantissa = new StringBuilder();
            mantissa.append(digits.charAt(0));
            if (digits.length() > 1) {
                mantissa.append('.').append(digits, 1, digits.length());
            }
            body = stripTrailingZeros(mantissa.toString())
                    + "e" + String.format(Locale.ROOT, "%+03d", exponent);
        } else {
            body = stripTrailingZeros(rounded.toPlainString());
        }
        return negative ? "-" + body : body;
    }

    private static String stripTrailingZeros(String s) {
        if (s.indexOf('.') < 0) {
            return s;
        }
        int end = s.length();
        while (end > 0 && s.charAt(end - 1) == '0') {
            end--;
        }
        if (end > 0 && s.charAt(end - 1) == '.') {
            end--;
        }
        return s.substring(0, end);
    }
}
