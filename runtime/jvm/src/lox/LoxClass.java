package lox;

import java.util.LinkedHashMap;
import java.util.Map;

/**
 * A class value. INHERIT copies the superclass's methods down at declaration
 * time (vm.cpp: {@code subclass->methods.addAll(superclass->methods)}), so a
 * subclass's own DEFINE_METHOD calls simply overwrite matching names here —
 * there is no runtime superclass-chain method lookup for a plain call.
 */
public final class LoxClass implements LoxCallable {
    public final String name;
    // Not final: INHERIT runs after CLASS already pushed and
    // stored this object (compiler.cpp emits CLASS before the superclass
    // clause is even parsed), so the superclass can only become known by
    // mutating the SAME object identity in place — see inheritFrom.
    public LoxClass superclass;
    public final Map<String, LoxClosure> methods = new LinkedHashMap<>();

    public LoxClass(String name, LoxClass superclass) {
        this.name = name;
        this.superclass = superclass;
        if (superclass != null) {
            methods.putAll(superclass.methods);
        }
    }

    /**
     * INHERIT's mutation: copies the superclass's methods down and
     * records it, in place, matching vm.cpp's {@code subclass->methods.
     * addAll(superclass->methods); subclass->superclass = superclass;}
     * exactly. Every later GET_GLOBAL/GET_LOCAL of this same class — the
     * compiler re-reads the class value right after INHERIT so DEFINE_METHOD
     * can find it — must see the merged state through this one identity.
     */
    public void inheritFrom(LoxClass superclass) {
        this.superclass = superclass;
        methods.putAll(superclass.methods);
    }

    public LoxClosure findMethod(String name) {
        return methods.get(name);
    }

    /**
     * Calling a class allocates an instance and runs `init` if the class (or
     * an ancestor) defines one. `init`'s return value is ignored: an
     * initializer always yields the receiver, never a computed value (a
     * bare `return;` still produces the instance; `return <expr>;` is a
     * compile error — see bytecode-translation-problems.md).
     */
    @Override
    public Object call(Object[] args) {
        LoxInstance instance = new LoxInstance(this);
        LoxClosure init = findMethod("init");
        if (init != null) {
            init.callAsSelf(instance, args);
        } else if (args.length != 0) {
            throw new LoxError("Expected 0 arguments but got " + args.length + ".");
        }
        return instance;
    }
}
