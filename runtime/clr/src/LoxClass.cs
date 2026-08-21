using System.Collections.Generic;

namespace Lox;

/// <summary>
/// A class value. INHERIT copies the superclass's methods down at
/// declaration time (vm.cpp: <c>subclass-&gt;methods.addAll(superclass-&gt;methods)</c>),
/// so a subclass's own DEFINE_METHOD calls simply overwrite matching names
/// here - there is no runtime superclass-chain method lookup for a plain
/// call.
/// </summary>
public sealed class LoxClass : ILoxCallable {
    public readonly string Name;

    // Not readonly: INHERIT runs after CLASS already pushed and stored this
    // object (compiler.cpp emits CLASS before the superclass clause is even
    // parsed), so the superclass can only become known by mutating the SAME
    // object identity in place - see InheritFrom.
    public LoxClass Superclass;
    public readonly Dictionary<string, LoxClosure> Methods = new();

    public LoxClass(string name, LoxClass superclass) {
        Name = name;
        Superclass = superclass;
        if (superclass != null) {
            foreach (var entry in superclass.Methods) {
                Methods[entry.Key] = entry.Value;
            }
        }
    }

    /// <summary>
    /// INHERIT's mutation: copies the superclass's methods down and records
    /// it, in place, matching vm.cpp's <c>subclass-&gt;methods.addAll(superclass-&gt;methods);
    /// subclass-&gt;superclass = superclass;</c> exactly. Every later
    /// GET_GLOBAL/GET_LOCAL of this same class - the compiler re-reads the
    /// class value right after INHERIT so DEFINE_METHOD can find it - must
    /// see the merged state through this one identity.
    /// </summary>
    public void InheritFrom(LoxClass superclass) {
        Superclass = superclass;
        foreach (var entry in superclass.Methods) {
            Methods[entry.Key] = entry.Value;
        }
    }

    public LoxClosure FindMethod(string name) {
        return Methods.TryGetValue(name, out LoxClosure method) ? method : null;
    }

    /// <summary>
    /// Calling a class allocates an instance and runs <c>init</c> if the
    /// class (or an ancestor) defines one. <c>init</c>'s return value is
    /// ignored: an initializer always yields the receiver, never a computed
    /// value (a bare <c>return;</c> still produces the instance;
    /// <c>return &lt;expr&gt;;</c> is a compile error - see
    /// bytecode-translation-problems.md).
    /// </summary>
    public object Call(object[] args) {
        var instance = new LoxInstance(this);
        LoxClosure init = FindMethod("init");
        if (init != null) {
            init.CallAsSelf(instance, args);
        } else if (args.Length != 0) {
            throw new LoxError($"Expected 0 arguments but got {args.Length}.");
        }
        return instance;
    }
}
