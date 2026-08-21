using System;
using System.Collections.Generic;
using System.Globalization;
using System.Numerics;
using System.Text;

namespace Lox;

/// <summary>
/// Static helpers for every opcode that is not pure stack motion. Semantics
/// come from src/vm.cpp; error text is copied where practical, but the
/// differential gate compares stdout only, so wording is not
/// load-bearing - only the value each helper produces is.
/// </summary>
public static class LoxOps {
    // ------------------------------------------------------------------
    // Truthiness (operator! in value.h: numbers and objects are truthy)
    // ------------------------------------------------------------------

    public static bool IsFalsy(object v) {
        if (v is bool b) {
            return !b;
        }
        return v == null;
    }

    public static object Not(object v) => IsFalsy(v);

    // ------------------------------------------------------------------
    // Arithmetic
    // ------------------------------------------------------------------

    public static double CheckNumber(object v) {
        if (v is not double d) {
            throw new LoxError("Operand must be a number.");
        }
        return d;
    }

    private static void CheckNumbers(object a, object b) {
        if (a is not double || b is not double) {
            throw new LoxError("Operands must be numbers.");
        }
    }

    public static object Add(object a, object b) {
        if (a is string sa && b is string sb) {
            return sa + sb;
        }
        CheckNumbers(a, b);
        return (double)a + (double)b;
    }

    public static object Subtract(object a, object b) {
        CheckNumbers(a, b);
        return (double)a - (double)b;
    }

    public static object Multiply(object a, object b) {
        CheckNumbers(a, b);
        return (double)a * (double)b;
    }

    public static object Divide(object a, object b) {
        CheckNumbers(a, b);
        return (double)a / (double)b;
    }

    /// <summary>Floor-division sign: the result takes the sign of `b` (Python/Lua rule), matching vm.cpp exactly.</summary>
    public static object Modulo(object a, object b) {
        CheckNumbers(a, b);
        double bd = (double)b;
        double result = (double)a % bd; // C#'s `%` on doubles is fmod, matching C's fmod
        if (result != 0 && (result < 0) != (bd < 0)) {
            result += bd;
        }
        return result;
    }

    public static object Negate(object a) => -CheckNumber(a);

    // ------------------------------------------------------------------
    // Comparisons
    // ------------------------------------------------------------------

    /// <summary>
    /// Strings compare by content, not identity: the native VM relies on
    /// interning (equal content -&gt; same ObjString pointer) to fold string
    /// equality into its generic Obj* identity check. This runtime does not
    /// intern computed strings (ADD produces a fresh <see cref="string"/>
    /// each time), so string equality is special-cased here instead.
    /// </summary>
    public static bool Equal(object a, object b) {
        if (a is double da && b is double db) {
            return da == db;
        }
        if (a is double || b is double) {
            return false;
        }
        if (a is bool ba && b is bool bb) {
            return ba == bb;
        }
        if (a is string sa && b is string sb) {
            return sa == sb;
        }
        // A map/file method value is a fresh, per-instance closure here (no
        // class-wide method table to share, unlike src/vm.cpp's ObjClass),
        // so its identity is carried in its name instead - see LoxMapMethod
        // and LoxFileMethod.
        if (a is LoxMapMethod ma && b is LoxMapMethod mb) {
            return ma.Name == mb.Name;
        }
        if (a is LoxFileMethod fa && b is LoxFileMethod fb) {
            return fa.Name == fb.Name;
        }
        return ReferenceEquals(a, b); // nil (null == null) and every identity-equality object type
    }

    public static bool Greater(object a, object b) {
        CheckNumbers(a, b);
        return (double)a > (double)b;
    }

    public static bool Less(object a, object b) {
        CheckNumbers(a, b);
        return (double)a < (double)b;
    }

    // ------------------------------------------------------------------
    // Sequences: in / slice / index
    // ------------------------------------------------------------------

    /// <summary>Public for codegen's BUILD_MAP: every key must pass this before the map is built, per vm.cpp.</summary>
    public static void CheckMapKey(object key) {
        if (key == null || key is bool || key is string) {
            return;
        }
        if (key is double d && !double.IsNaN(d)) {
            return;
        }
        throw new LoxError("Map keys must be Bool, Number, Nil, or String. NaN is not allowed.");
    }

    /// <summary>Operand order matches the on-stack order (elem below seq) - chunk.h's IN pops [elem, seq].</summary>
    public static bool In(object elem, object seq) {
        if (seq is LoxList list) {
            foreach (object v in list.Elements) {
                if (Equal(v, elem)) {
                    return true;
                }
            }
            return false;
        }
        if (seq is string s) {
            if (elem is not string elemStr) {
                throw new LoxError("Left operand of 'in' on a string must be a string.");
            }
            return s.Contains(elemStr);
        }
        if (seq is LoxMap map) {
            CheckMapKey(elem);
            return map.Has(elem);
        }
        throw new LoxError("Right operand of 'in' must be a list, string, or map.");
    }

    /// <summary>Validates a slice index down to a non-negative integral double - no upper bound yet, matching Op::SLICE (src/vm.cpp) before the sequence length is known.</summary>
    private static double ValidateSliceIndex(object v) {
        if (v is not double d) {
            throw new LoxError("Slice index must be a number.");
        }
        if (d != Math.Floor(d)) {
            throw new LoxError("Slice index must be an integer.");
        }
        if (d < 0.0) {
            throw new LoxError("Slice index must be non-negative.");
        }
        return d;
    }

    /// <summary>
    /// Clamps in the double domain, matching Op::SLICE's
    /// <c>std::min(startD, (double)n)</c>, then converts to int. Casting to
    /// int before clamping (the previous bug here) is unchecked in C#: a
    /// double past int.MaxValue becomes an unrelated, possibly negative int
    /// instead of a clamped, in-range one.
    /// </summary>
    private static int ClampSliceIndex(double d, int size) {
        return (int)Math.Min(d, (double)size);
    }

    /// <summary>[seq, start, end] - the same parameter order the operand stack holds bottom-up.</summary>
    public static object Slice(object seq, object startVal, object endVal) {
        if (seq is not LoxList && seq is not string) {
            throw new LoxError("Slice requires a List or String.");
        }
        double startD = ValidateSliceIndex(startVal);
        double endD = ValidateSliceIndex(endVal);
        if (seq is LoxList list) {
            List<object> src = list.Elements;
            int n = src.Count;
            int s = ClampSliceIndex(startD, n);
            int e = ClampSliceIndex(endD, n);
            var result = new LoxList();
            if (s < e) {
                result.Elements.AddRange(src.GetRange(s, e - s));
            }
            return result;
        }
        string str = (string)seq;
        int strN = str.Length;
        int strS = ClampSliceIndex(startD, strN);
        int strE = ClampSliceIndex(endD, strN);
        return (strS < strE) ? str.Substring(strS, strE - strS) : "";
    }

    private static int BoundedIndex(object indexVal, int size, string kind) {
        if (indexVal is not double d) {
            throw new LoxError($"{kind} index must be a number.");
        }
        if (d != Math.Floor(d)) {
            throw new LoxError($"{kind} index must be an integer.");
        }
        int idx = (int)d;
        if (idx < 0 || idx >= size) {
            throw new LoxError($"{kind} index out of bounds.");
        }
        return idx;
    }

    /// <summary>
    /// BUILD_LIST's own helper. <paramref name="elements"/> arrives in
    /// first-to-last order already (vm.cpp's BUILD_LIST pops its operands
    /// top-to-bottom into a bottom-to-top array; the emitter mirrors that
    /// same order building this one, same as it already does for
    /// <see cref="Call"/>'s args array).
    /// </summary>
    public static LoxList BuildList(object[] elements) {
        var list = new LoxList();
        foreach (object e in elements) {
            list.Elements.Add(e);
        }
        return list;
    }

    /// <summary>
    /// BUILD_MAP's own helper, same P7 spill-then-materialize shape as
    /// <see cref="BuildList"/>, doubled: <paramref name="kv"/> holds
    /// <c>count * 2</c> elements, <c>[key0, val0, key1, val1, ...]</c>, in
    /// the same first-to-last order the emitter builds for
    /// <see cref="BuildList"/>'s own array. vm.cpp validates every key
    /// before writing any pair ("Validate all keys before any allocation");
    /// this keeps that same two-pass shape, so a bad key never leaves a
    /// partially-built map behind.
    /// </summary>
    public static LoxMap BuildMap(object[] kv) {
        for (int i = 0; i < kv.Length; i += 2) {
            CheckMapKey(kv[i]);
        }
        var map = new LoxMap();
        for (int i = 0; i < kv.Length; i += 2) {
            map.Put(kv[i], kv[i + 1]);
        }
        return map;
    }

    // BINDING INVARIANT: no Lox value may be a bare object[]. The captured-
    // local discriminator (the emitter's own runtime cell check) tells a
    // ref-cell from a raw value at run time with `is object[]` on whatever a
    // local slot holds. A bare object[] stored into a slot reads as a cell,
    // so every read/write of that slot in a captured local's program would
    // silently read/write its element 0 instead of the value itself - no
    // verifier error, no exception, just a wrong answer. Every aggregate
    // here is a named class instead (LoxList, LoxMap, LoxEnum, LoxClosure,
    // LoxInstance, LoxFile); the only object[] uses in this runtime are
    // LoxEnum.Payload and Call/BuildList/BuildMap's own argument arrays, and
    // none of those ever reaches a local slot. Wrap any new aggregate type
    // in a named class too, and see ObjectArrayInvariantTest for the
    // enforcement.
    public static object GetIndex(object collection, object index) {
        if (collection is LoxList list) {
            return list.Elements[BoundedIndex(index, list.Elements.Count, "List")];
        }
        if (collection is string s) {
            return s[BoundedIndex(index, s.Length, "String")].ToString();
        }
        if (collection is LoxMap map) {
            CheckMapKey(index);
            return map.Get(index); // absent key and a stored nil both read back as null
        }
        if (collection is LoxEnum e) {
            if (index is not double d) {
                throw new LoxError("Enum field index must be a number.");
            }
            object[] payload = e.Payload;
            int idx = (int)d;
            if (idx < 0 || idx >= payload.Length) {
                throw new LoxError($"Enum field index {idx} out of range.");
            }
            return payload[idx];
        }
        throw new LoxError("Only lists, strings, and maps can be indexed.");
    }

    public static object SetIndex(object collection, object index, object value) {
        if (collection is string) {
            throw new LoxError("Strings are immutable and cannot be indexed for assignment.");
        }
        if (collection is LoxMap map) {
            CheckMapKey(index);
            map.Put(index, value);
            return value;
        }
        if (collection is not LoxList list) {
            throw new LoxError("Only lists and maps can be indexed for assignment.");
        }
        list.Elements[BoundedIndex(index, list.Elements.Count, "List")] = value;
        return value; // assignment is an expression
    }

    public static LoxIterator GetIter(object iterable) {
        if (iterable is not LoxList && iterable is not string && iterable is not LoxMap) {
            throw new LoxError("Value is not iterable (expected list, string, or map).");
        }
        return new LoxIterator(iterable);
    }

    /// <summary>
    /// ITER_HAS_NEXT/ITER_NEXT operate on the copy a preceding GET_LOCAL
    /// already loaded (P8) - the iterator's own local slot is untouched,
    /// and neither opcode consumes it. A plain cast is enough: GET_ITER is
    /// the only producer of a <see cref="LoxIterator"/> value, so this cast
    /// can never see anything else.
    /// </summary>
    public static bool IterHasNext(object it) => ((LoxIterator)it).HasNext();

    public static object IterNext(object it) => ((LoxIterator)it).Next();

    /// <summary>Matches Op::IS_SEQ exactly: List and String only - Map is not included (vm.cpp).</summary>
    public static bool IsSeq(object v) => v is LoxList || v is string;

    /// <summary>
    /// Builds, but does not throw, the error. Codegen needs a real terminal
    /// instruction in the generated CIL, not only a call the verifier
    /// cannot prove always throws; it takes the returned object and follows
    /// it with a `throw` itself.
    /// </summary>
    public static LoxError MatchError() => new("MatchError: no matching arm.");

    // ------------------------------------------------------------------
    // instanceof / properties / methods
    // ------------------------------------------------------------------

    /// <summary>Never throws: an undefined or non-class name simply fails to match, as in vm.cpp's INSTANCEOF.</summary>
    public static bool InstanceOf(object val, LoxGlobals globals, string className) {
        if (val is not LoxInstance instance || !globals.IsDefined(className)) {
            return false;
        }
        if (globals.Get(className) is not LoxClass target) {
            return false;
        }
        LoxClass k = instance.Klass;
        while (k != null) {
            if (ReferenceEquals(k, target)) {
                return true;
            }
            k = k.Superclass;
        }
        return false;
    }

    public static object GetProperty(object obj, string name) {
        if (obj is LoxFile file) {
            ILoxCallable m = file.GetMethod(name);
            if (m == null) {
                throw new LoxError($"Undefined property '{name}' on file.");
            }
            return m;
        }
        if (obj is LoxMap map) {
            ILoxCallable m = map.GetMethod(name);
            if (m == null) {
                throw new LoxError($"Undefined property '{name}' on map.");
            }
            return m;
        }
        if (obj is not LoxInstance instance) {
            throw new LoxError("Only instances have properties.");
        }
        if (instance.Fields.TryGetValue(name, out object fieldVal)) {
            return fieldVal;
        }
        LoxClosure method = instance.Klass.FindMethod(name);
        if (method == null) {
            throw new LoxError($"Undefined property '{name}'.");
        }
        return new LoxBoundMethod(instance, method);
    }

    public static object SetProperty(object obj, string name, object value) {
        if (obj is not LoxInstance instance) {
            throw new LoxError("Only instances have fields.");
        }
        instance.Fields[name] = value;
        return value; // assignment is an expression
    }

    public static void DefineMethod(LoxClass klass, string name, LoxClosure method) {
        klass.Methods[name] = method;
    }

    /// <summary>
    /// The INHERIT opcode's guard. Codegen would otherwise need a raw cast
    /// to <see cref="LoxClass"/>, which throws <see cref="InvalidCastException"/>
    /// instead of <see cref="LoxError"/>. Call this before constructing the
    /// subclass, so its superclass is already validated.
    /// </summary>
    public static LoxClass Inherit(object superclassVal) {
        if (superclassVal is not LoxClass superclass) {
            throw new LoxError("Superclass must be a class.");
        }
        return superclass;
    }

    /// <summary>
    /// The INHERIT opcode's own codegen entry point: mutates
    /// <paramref name="subclassVal"/> in place through
    /// <see cref="LoxClass.InheritFrom"/> - see that method's own note for
    /// why a reconstructed object cannot stand in for it. Reuses
    /// <see cref="Inherit"/> for the exact same validation and error text
    /// as the guard above. Parameter order (subclass, then superclass)
    /// matches the codegen call site's own push order.
    /// </summary>
    public static void InheritInto(object subclassVal, object superclassVal) {
        LoxClass superclass = Inherit(superclassVal);
        ((LoxClass)subclassVal).InheritFrom(superclass);
    }

    /// <summary>The GET_TAG opcode's guard - same cast-exception problem as <see cref="Inherit"/>.</summary>
    public static double GetTag(object v) {
        if (v is not LoxEnum e) {
            throw new LoxError("GET_TAG: expected an enum value.");
        }
        return e.Ctor.Tag;
    }

    /// <summary>
    /// The CALL opcode's runtime dispatch (P6, vm.cpp callValue): a closure,
    /// a native, a bound method, a class (construction), and an enum
    /// constructor all already implement <see cref="ILoxCallable"/>, so one
    /// type test covers every kind vm.cpp's callValue switches on. Generated
    /// code never branches on the callee's kind - this is the one place
    /// that does.
    /// </summary>
    public static object Call(object callee, object[] args) {
        if (callee is ILoxCallable callable) {
            return callable.Call(args);
        }
        throw new LoxError("Can only call functions, classes and enums.");
    }

    /// <summary>The INVOKE fast path: dispatches on the receiver's runtime kind (P6), not on one static type.</summary>
    public static object Invoke(object receiver, string name, object[] args) {
        if (receiver is LoxInstance instance) {
            if (instance.Fields.TryGetValue(name, out object fieldVal)) {
                // vm.cpp calls only a closure or a native field this way; a
                // class, an enum constructor, or a bound method is a
                // runtime error here, even though all four implement
                // ILoxCallable.
                if (fieldVal is LoxClosure closure) {
                    return closure.Call(args);
                }
                if (fieldVal is LoxNative native) {
                    return native.Call(args);
                }
                throw new LoxError("Can only call functions, classes and enums.");
            }
            LoxClosure method = instance.Klass.FindMethod(name);
            if (method == null) {
                throw new LoxError($"Undefined property '{name}'.");
            }
            return method.CallAsSelf(instance, args);
        }
        if (receiver is LoxList list) {
            return InvokeListMethod(list, name, args);
        }
        if (receiver is LoxFile file) {
            return InvokeFileMethod(file, name, args);
        }
        if (receiver is LoxMap map) {
            return InvokeMapMethod(map, name, args);
        }
        throw new LoxError("Only instances, files, and maps have methods.");
    }

    private static object InvokeListMethod(LoxList list, string name, object[] args) {
        switch (name) {
        case "append":
            if (args.Length != 1) {
                throw new LoxError($"'append' expects 1 argument but got {args.Length}.");
            }
            list.Elements.Add(args[0]);
            return null;
        case "pop":
            if (args.Length != 0) {
                throw new LoxError($"'pop' expects 0 arguments but got {args.Length}.");
            }
            if (list.Elements.Count == 0) {
                throw new LoxError("Cannot pop from an empty list.");
            }
            object last = list.Elements[^1];
            list.Elements.RemoveAt(list.Elements.Count - 1);
            return last;
        case "remove":
            if (args.Length != 1) {
                throw new LoxError($"'remove' expects 1 argument but got {args.Length}.");
            }
            for (int i = 0; i < list.Elements.Count; i++) {
                if (Equal(list.Elements[i], args[0])) {
                    list.Elements.RemoveAt(i);
                    return null;
                }
            }
            throw new LoxError("Value not found in list.");
        default:
            throw new LoxError($"Undefined method '{name}' on list.");
        }
    }

    /// <summary>
    /// Dispatches by name with no LoxNative allocation - the INVOKE fast
    /// path matches vm.cpp's own fast path, which calls the native C
    /// function directly and never builds an intermediate ObjNative per
    /// call. <see cref="LoxMap.GetMethod"/> still allocates (once, cached)
    /// for the separate GET_PROPERTY case.
    /// </summary>
    private static object InvokeMapMethod(LoxMap map, string name, object[] args) {
        switch (name) {
        case "has":
            RequireArity(args, 1, "has");
            CheckMapKey(args[0]);
            return map.Has(args[0]);
        case "del":
            RequireArity(args, 1, "del");
            CheckMapKey(args[0]);
            map.Remove(args[0]);
            return null;
        case "keys": {
            RequireArity(args, 0, "keys");
            var list = new LoxList();
            foreach (var e in map.Entries()) {
                list.Elements.Add(e.Key);
            }
            return list;
        }
        case "values": {
            RequireArity(args, 0, "values");
            var list = new LoxList();
            foreach (var e in map.Entries()) {
                list.Elements.Add(e.Value);
            }
            return list;
        }
        case "entries": {
            RequireArity(args, 0, "entries");
            var list = new LoxList();
            foreach (var e in map.Entries()) {
                var pair = new LoxList();
                pair.Elements.Add(e.Key);
                pair.Elements.Add(e.Value);
                list.Elements.Add(pair);
            }
            return list;
        }
        default:
            throw new LoxError($"Undefined method '{name}' on map.");
        }
    }

    /// <summary>Same no-allocation dispatch as <see cref="InvokeMapMethod"/>, for the file API.</summary>
    private static object InvokeFileMethod(LoxFile file, string name, object[] args) {
        switch (name) {
        case "read":
            RequireArity(args, 0, "read");
            return file.Read();
        case "readline":
            RequireArity(args, 0, "readline");
            return file.Readline();
        case "readlines":
            RequireArity(args, 0, "readlines");
            return file.Readlines();
        case "write":
            RequireArity(args, 1, "write");
            file.Write(CheckStringArg(args[0], "write"));
            return null;
        case "writeline":
            RequireArity(args, 1, "writeline");
            file.Writeline(CheckStringArg(args[0], "writeline"));
            return null;
        case "close":
            RequireArity(args, 0, "close");
            file.Close();
            return null;
        default:
            throw new LoxError($"Undefined method '{name}' on file.");
        }
    }

    private static void RequireArity(object[] args, int arity, string method) {
        if (args.Length != arity) {
            throw new LoxError($"'{method}' expects {arity} argument(s) but got {args.Length}.");
        }
    }

    private static string CheckStringArg(object v, string method) {
        if (v is not string s) {
            throw new LoxError($"'{method}' argument must be a string.");
        }
        return s;
    }

    public static object GetSuper(object superclassVal, string name, object self) {
        LoxClosure method = ((LoxClass)superclassVal).FindMethod(name);
        if (method == null) {
            throw new LoxError($"Undefined property '{name}'.");
        }
        return new LoxBoundMethod(self, method);
    }

    public static object SuperInvoke(object superclassVal, string name, object self, object[] args) {
        LoxClosure method = ((LoxClass)superclassVal).FindMethod(name);
        if (method == null) {
            throw new LoxError($"Undefined property '{name}'.");
        }
        return method.CallAsSelf(self, args);
    }

    // ------------------------------------------------------------------
    // print / stringify
    // ------------------------------------------------------------------

    public static void Print(object v) {
        LoxRuntime.Out.Write(Stringify(v));
        LoxRuntime.Out.Write('\n');
    }

    public static string Stringify(object v) {
        if (v == null) {
            return "nil";
        }
        if (v is bool b) {
            return b ? "true" : "false";
        }
        if (v is double d) {
            return FormatNumber(d);
        }
        if (v is string s) {
            return s;
        }
        if (v is LoxClosure closure) {
            return closure.Name == null ? "<script>" : $"<fn {closure.Name}>";
        }
        if (v is LoxNative) {
            return "<native fn>";
        }
        if (v is LoxBoundMethod bound) {
            return $"<fn {bound.Method.Name}>";
        }
        if (v is LoxClass klass) {
            return klass.Name;
        }
        if (v is LoxInstance instance) {
            return $"{instance.Klass.Name} instance";
        }
        if (v is LoxFile) {
            return "<file>";
        }
        if (v is LoxIterator) {
            return "<iterator>";
        }
        if (v is LoxList list) {
            var sb = new StringBuilder("[");
            for (int i = 0; i < list.Elements.Count; i++) {
                if (i > 0) {
                    sb.Append(", ");
                }
                sb.Append(Stringify(list.Elements[i]));
            }
            return sb.Append(']').ToString();
        }
        if (v is LoxMap map) {
            var sb = new StringBuilder("{");
            bool first = true;
            foreach (var e in map.Entries()) {
                if (!first) {
                    sb.Append(", ");
                }
                first = false;
                sb.Append(Stringify(e.Key)).Append(": ").Append(Stringify(e.Value));
            }
            return sb.Append('}').ToString();
        }
        if (v is LoxEnumCtor ctor) {
            return $"<ctor {ctor.EnumName}::{ctor.CtorName}>";
        }
        if (v is LoxEnum e2) {
            var sb = new StringBuilder(e2.Ctor.EnumName).Append("::").Append(e2.Ctor.CtorName);
            if (e2.Payload.Length > 0) {
                sb.Append('(');
                for (int i = 0; i < e2.Payload.Length; i++) {
                    if (i > 0) {
                        sb.Append(", ");
                    }
                    sb.Append(Stringify(e2.Payload[i]));
                }
                sb.Append(')');
            }
            return sb.ToString();
        }
        throw new InvalidOperationException($"stringify: unrecognized value type {v.GetType()}");
    }

    /// <summary>
    /// Reproduces C's <c>snprintf("%g", d)</c> byte for byte (6 significant
    /// digits, trailing zeros stripped, <c>e±0N</c> exponents) - see
    /// spec/03-types.md's Number row and value.cpp's stringify. .NET's
    /// double formatting (with or without a format string) uses a
    /// different algorithm entirely (e.g. it never strips to a bare
    /// integer, and switches to scientific notation on a different
    /// threshold), so it cannot be used here. Instead this expands the
    /// double's EXACT binary value into decimal digits with
    /// <see cref="BigInteger"/> (mantissa * 2^exponent, or, for a negative
    /// exponent, mantissa * 5^-exponent scaled by 10^-exponent) and rounds
    /// that exact value to 6 significant digits with round-half-to-even -
    /// the same correctly-rounded conversion snprintf performs, not a
    /// round-trip through the shortest decimal string.
    /// </summary>
    private static string FormatNumber(double d) {
        if (double.IsNaN(d)) {
            // glibc's printf prints the sign bit of the NaN payload itself;
            // an indeterminate 0.0/0.0 on x86_64 carries a set sign bit
            // (confirmed against build/loxpp, matching this CLR's runtime
            // division on the same hardware), hence "-nan" rather than "nan".
            return BitConverter.DoubleToInt64Bits(d) < 0 ? "-nan" : "nan";
        }
        if (double.IsPositiveInfinity(d)) {
            return "inf";
        }
        if (double.IsNegativeInfinity(d)) {
            return "-inf";
        }
        bool negative = BitConverter.DoubleToInt64Bits(d) < 0;
        double mag = Math.Abs(d);
        if (mag == 0.0) {
            return negative ? "-0" : "0";
        }

        (string digits, int scale) = ExactDecimal(mag);
        (string roundedDigits, int roundedScale) = RoundToSignificantDigits(digits, scale, 6);
        int exponent = roundedDigits.Length - roundedScale - 1; // %g's X

        string body;
        if (exponent < -4 || exponent >= 6) {
            var mantissa = new StringBuilder();
            mantissa.Append(roundedDigits[0]);
            if (roundedDigits.Length > 1) {
                mantissa.Append('.').Append(roundedDigits, 1, roundedDigits.Length - 1);
            }
            body = StripTrailingZeros(mantissa.ToString()) + "e" + FormatExponent(exponent);
        } else {
            body = StripTrailingZeros(PlainString(roundedDigits, roundedScale));
        }
        return negative ? "-" + body : body;
    }

    /// <summary>
    /// The exact decimal value of a finite, positive, non-zero double, as
    /// (digit string with no leading zeros, scale) such that the value
    /// equals <c>digits * 10^-scale</c> - the same (unscaledValue, scale)
    /// shape java.math.BigDecimal(double) uses internally, derived directly
    /// from the IEEE 754 bit layout rather than through a library.
    /// </summary>
    private static (string digits, int scale) ExactDecimal(double mag) {
        long bits = BitConverter.DoubleToInt64Bits(mag);
        int biasedExponent = (int)((bits >> 52) & 0x7FFL);
        long mantissaBits = bits & 0xFFFFFFFFFFFFFL;
        BigInteger mantissa;
        int e;
        if (biasedExponent == 0) {
            mantissa = mantissaBits; // subnormal: no implicit leading bit
            e = 1 - 1023 - 52;
        } else {
            mantissa = mantissaBits | (1L << 52);
            e = biasedExponent - 1023 - 52;
        }

        BigInteger unscaled;
        int scale;
        if (e >= 0) {
            unscaled = mantissa * BigInteger.Pow(2, e);
            scale = 0;
        } else {
            int n = -e;
            unscaled = mantissa * BigInteger.Pow(5, n);
            scale = n;
        }
        return (unscaled.ToString(CultureInfo.InvariantCulture), scale);
    }

    /// <summary>
    /// Rounds an exact (digits, scale) decimal pair to at most
    /// <paramref name="sigDigits"/> significant digits, round-half-to-even.
    /// Leaves it unchanged if it already has <paramref name="sigDigits"/>
    /// or fewer.
    /// </summary>
    private static (string digits, int scale) RoundToSignificantDigits(string digits, int scale, int sigDigits) {
        int numDigits = digits.Length;
        if (numDigits <= sigDigits) {
            return (digits, scale);
        }
        int digitsToRemove = numDigits - sigDigits;
        BigInteger unscaled = BigInteger.Parse(digits, CultureInfo.InvariantCulture);
        BigInteger divisor = BigInteger.Pow(10, digitsToRemove);
        BigInteger quotient = BigInteger.DivRem(unscaled, divisor, out BigInteger remainder);
        BigInteger twiceRemainder = remainder * 2;
        if (twiceRemainder > divisor || (twiceRemainder == divisor && !quotient.IsEven)) {
            quotient += 1;
        }
        // Rounding up can carry into one extra digit (e.g. 999999.5... ->
        // 1000000): fold it back into sigDigits digits by dropping one more
        // trailing zero, matching BigDecimal.round's own carry handling.
        if (quotient == BigInteger.Pow(10, sigDigits)) {
            quotient /= 10;
            digitsToRemove += 1;
        }
        return (quotient.ToString(CultureInfo.InvariantCulture), scale - digitsToRemove);
    }

    /// <summary>Reconstructs the plain (non-exponential) decimal string for a (digits, scale) pair.</summary>
    private static string PlainString(string digits, int scale) {
        int pointPos = digits.Length - scale;
        if (pointPos <= 0) {
            return "0." + new string('0', -pointPos) + digits;
        }
        if (pointPos >= digits.Length) {
            return digits + new string('0', pointPos - digits.Length);
        }
        return digits.Substring(0, pointPos) + "." + digits.Substring(pointPos);
    }

    private static string FormatExponent(int exponent) {
        string sign = exponent >= 0 ? "+" : "-";
        return sign + Math.Abs(exponent).ToString("D2", CultureInfo.InvariantCulture);
    }

    private static string StripTrailingZeros(string s) {
        int dot = s.IndexOf('.');
        if (dot < 0) {
            return s;
        }
        int end = s.Length;
        while (end > 0 && s[end - 1] == '0') {
            end--;
        }
        if (end > 0 && s[end - 1] == '.') {
            end--;
        }
        return s.Substring(0, end);
    }
}
