package lox;

import static lox.TestSupport.check;
import static lox.TestSupport.checkEquals;
import static lox.TestSupport.checkThrows;

public final class ClassesTest {
    /** A closure with no captured state: `fun add(a, b) { return a + b; }`. */
    private static LoxClosure addClosure() {
        return new LoxClosure("add", 2, new Object[0][]) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                return LoxOps.add(a[0], a[1]);
            }
        };
    }

    public static void main(String[] args) {
        LoxClosure add = addClosure();
        checkEquals(3.0, add.call(new Object[] {1.0, 2.0}), "closure.call runs invoke");
        checkThrows(() -> add.call(new Object[] {1.0}), LoxError.class, "closure arity check");

        Object[][] upvalues = {{10.0}};
        LoxClosure withUpvalue = new LoxClosure("counter", 0, upvalues) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                return upvalues[0][0];
            }
        };
        checkEquals(10.0, withUpvalue.call(new Object[0]), "closure reads its upvalue cell");
        upvalues[0][0] = 20.0; // CLOSE_UPVALUE's fresh-cell model: mutate through the shared cell
        checkEquals(20.0, withUpvalue.call(new Object[0]), "closure observes a mutation through its cell");

        // `class Animal { speak() { return "..."; } }`
        LoxClass animal = new LoxClass("Animal", null);
        LoxClosure speak = new LoxClosure("speak", 0, new Object[0][]) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                return "...";
            }
        };
        LoxOps.defineMethod(animal, "speak", speak);

        // `class Dog < Animal { init(name) { this.name = name; } }`
        LoxClass dog = new LoxClass("Dog", animal);
        LoxClosure init = new LoxClosure("init", 1, new Object[0][]) {
            @Override
            protected Object invoke(Object self, Object[] a) {
                LoxOps.setProperty(self, "name", a[0]);
                return self;
            }
        };
        LoxOps.defineMethod(dog, "init", init);

        Object rex = dog.call(new Object[] {"Rex"});
        check(rex instanceof LoxInstance, "calling a class returns an instance");
        checkEquals("Rex", LoxOps.getProperty(rex, "name"), "init ran and set a field");
        checkEquals("...", LoxOps.invoke(rex, "speak", new Object[0]), "INVOKE finds an inherited method");
        checkThrows(() -> dog.call(new Object[] {"a", "b"}), LoxError.class, "init arity is enforced");

        LoxClass noInit = new LoxClass("Plain", null);
        Object plain = noInit.call(new Object[0]);
        check(plain instanceof LoxInstance, "a class with no init still constructs");
        checkThrows(() -> noInit.call(new Object[] {1.0}), LoxError.class,
                "a class with no init rejects arguments");

        Object bound = LoxOps.getProperty(rex, "speak");
        check(bound instanceof LoxBoundMethod, "GET_PROPERTY on a method returns a BoundMethod");
        checkEquals("...", ((LoxCallable) bound).call(new Object[0]), "calling the bound method runs it");

        Object superBound = LoxOps.getSuper(animal, "speak", rex);
        checkEquals("...", ((LoxCallable) superBound).call(new Object[0]), "GET_SUPER binds the receiver");
        checkEquals("...", LoxOps.superInvoke(animal, "speak", rex, new Object[0]), "SUPER_INVOKE runs directly");
        checkThrows(() -> LoxOps.getSuper(animal, "missing", rex), LoxError.class,
                "GET_SUPER on an undefined method");

        LoxGlobals globals = new LoxGlobals();
        globals.define("Dog", dog);
        check(LoxOps.instanceOf(rex, globals, "Dog"), "instanceOf matches its own class");
        check(LoxOps.instanceOf(rex, globals, "Dog"), "instanceOf walks to itself first");
        globals.define("Animal", animal);
        check(LoxOps.instanceOf(rex, globals, "Animal"), "instanceOf walks the superclass chain");
        check(!LoxOps.instanceOf(rex, globals, "Missing"), "instanceOf never throws on an undefined name");
        check(!LoxOps.instanceOf(1.0, globals, "Dog"), "instanceOf is false for a non-instance value");

        checkThrows(() -> globals.get("undefined_name"), LoxError.class, "LoxGlobals.get(undefined) throws");
        checkThrows(() -> globals.set("undefined_name", 1.0), LoxError.class, "LoxGlobals.set(undefined) throws");
        check(!globals.isDefined("undefined_name"), "isDefined is false for an undefined name");
        globals.define("nilGlobal", null);
        check(globals.isDefined("nilGlobal"), "isDefined is true even when the value is nil");
        checkEquals(null, globals.get("nilGlobal"), "a nil-valued global reads back as nil, not undefined");
        globals.set("nilGlobal", 5.0);
        checkEquals(5.0, globals.get("nilGlobal"), "set updates an existing global");

        LoxEnumCtor ok = new LoxEnumCtor(1, 1, "Ok", "Result");
        Object a = ok.call(new Object[] {5.0});
        Object b = ok.call(new Object[] {5.0});
        check(!LoxOps.equal(a, b), "two enum values with the same tag and payload are not equal");
        check(LoxOps.equal(a, a), "an enum value equals itself");
        checkThrows(() -> ok.call(new Object[0]), LoxError.class, "enum constructor arity is enforced");

        // R5 (PR #97 review): codegen-facing guards for INHERIT and GET_TAG.
        checkThrows(() -> LoxOps.inherit(1.0), LoxError.class, "inherit() rejects a non-class value");
        check(LoxOps.inherit(animal) == animal, "inherit() returns the validated superclass");
        checkThrows(() -> LoxOps.getTag(1.0), LoxError.class, "getTag() rejects a non-enum value");
        checkEquals(1.0, LoxOps.getTag(a), "getTag() reads the constructor's tag as a Lox number");
        LoxOps.checkMapKey("k"); // public for N6's BUILD_MAP — must not throw for a valid key
        checkThrows(() -> LoxOps.checkMapKey(Double.NaN), LoxError.class, "checkMapKey() rejects NaN");

        System.exit(TestSupport.finish("ClassesTest"));
    }
}
