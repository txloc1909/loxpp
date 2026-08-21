using Lox;

namespace LoxRuntimeTests;

public static class ClassesTest {
    /// <summary>A closure with no captured state: `fun add(a, b) { return a + b; }`.</summary>
    private static DelegateClosure AddClosure() {
        return new DelegateClosure("add", 2, new object[0][], (self, a) => LoxOps.Add(a[0], a[1]));
    }

    public static int Run() {
        var t = new TestSupport();

        DelegateClosure add = AddClosure();
        t.CheckEquals(3.0, add.Call(new object[] { 1.0, 2.0 }), "closure.call runs invoke");
        t.CheckThrows(() => add.Call(new object[] { 1.0 }), typeof(LoxError), "closure arity check");

        object[][] upvalues = { new object[] { 10.0 } };
        var withUpvalue = new DelegateClosure("counter", 0, upvalues, (self, a) => upvalues[0][0]);
        t.CheckEquals(10.0, withUpvalue.Call(System.Array.Empty<object>()), "closure reads its upvalue cell");
        upvalues[0][0] = 20.0; // CLOSE_UPVALUE's fresh-cell model: mutate through the shared cell
        t.CheckEquals(20.0, withUpvalue.Call(System.Array.Empty<object>()), "closure observes a mutation through its cell");

        // `class Animal { speak() { return "..."; } }`
        var animal = new LoxClass("Animal", null);
        var speak = new DelegateClosure("speak", 0, new object[0][], (self, a) => "...");
        LoxOps.DefineMethod(animal, "speak", speak);

        // `class Dog < Animal { init(name) { this.name = name; } }`
        var dog = new LoxClass("Dog", animal);
        var init = new DelegateClosure("init", 1, new object[0][], (self, a) => {
            LoxOps.SetProperty(self, "name", a[0]);
            return self;
        });
        LoxOps.DefineMethod(dog, "init", init);

        object rex = dog.Call(new object[] { "Rex" });
        t.Check(rex is LoxInstance, "calling a class returns an instance");
        t.CheckEquals("Rex", LoxOps.GetProperty(rex, "name"), "init ran and set a field");
        t.CheckEquals("...", LoxOps.Invoke(rex, "speak", System.Array.Empty<object>()), "INVOKE finds an inherited method");
        t.CheckThrows(() => dog.Call(new object[] { "a", "b" }), typeof(LoxError), "init arity is enforced");

        var noInit = new LoxClass("Plain", null);
        object plain = noInit.Call(System.Array.Empty<object>());
        t.Check(plain is LoxInstance, "a class with no init still constructs");
        t.CheckThrows(() => noInit.Call(new object[] { 1.0 }), typeof(LoxError),
            "a class with no init rejects arguments");

        object bound = LoxOps.GetProperty(rex, "speak");
        t.Check(bound is LoxBoundMethod, "GET_PROPERTY on a method returns a BoundMethod");
        t.CheckEquals("...", ((ILoxCallable)bound).Call(System.Array.Empty<object>()), "calling the bound method runs it");

        object superBound = LoxOps.GetSuper(animal, "speak", rex);
        t.CheckEquals("...", ((ILoxCallable)superBound).Call(System.Array.Empty<object>()), "GET_SUPER binds the receiver");
        t.CheckEquals("...", LoxOps.SuperInvoke(animal, "speak", rex, System.Array.Empty<object>()), "SUPER_INVOKE runs directly");
        t.CheckThrows(() => LoxOps.GetSuper(animal, "missing", rex), typeof(LoxError),
            "GET_SUPER on an undefined method");

        var globals = new LoxGlobals();
        globals.Define("Dog", dog);
        t.Check(LoxOps.InstanceOf(rex, globals, "Dog"), "instanceOf matches its own class");
        t.Check(LoxOps.InstanceOf(rex, globals, "Dog"), "instanceOf walks to itself first");
        globals.Define("Animal", animal);
        t.Check(LoxOps.InstanceOf(rex, globals, "Animal"), "instanceOf walks the superclass chain");
        t.Check(!LoxOps.InstanceOf(rex, globals, "Missing"), "instanceOf never throws on an undefined name");
        t.Check(!LoxOps.InstanceOf(1.0, globals, "Dog"), "instanceOf is false for a non-instance value");

        t.CheckThrows(() => globals.Get("undefined_name"), typeof(LoxError), "LoxGlobals.get(undefined) throws");
        t.CheckThrows(() => globals.Set("undefined_name", 1.0), typeof(LoxError), "LoxGlobals.set(undefined) throws");
        t.Check(!globals.IsDefined("undefined_name"), "isDefined is false for an undefined name");
        globals.Define("nilGlobal", null);
        t.Check(globals.IsDefined("nilGlobal"), "isDefined is true even when the value is nil");
        t.CheckEquals(null, globals.Get("nilGlobal"), "a nil-valued global reads back as nil, not undefined");
        globals.Set("nilGlobal", 5.0);
        t.CheckEquals(5.0, globals.Get("nilGlobal"), "set updates an existing global");
        globals.Set("nilGlobal", null); // sentinel round-trip must survive a defined -> nil write
        t.CheckEquals(null, globals.Get("nilGlobal"), "set can write nil back onto a defined global");
        t.Check(globals.IsDefined("nilGlobal"), "isDefined stays true after set writes nil");

        var ok = new LoxEnumCtor(1, 1, "Ok", "Result");
        object a = ok.Call(new object[] { 5.0 });
        object b = ok.Call(new object[] { 5.0 });
        t.Check(!LoxOps.Equal(a, b), "two enum values with the same tag and payload are not equal");
        t.Check(LoxOps.Equal(a, a), "an enum value equals itself");
        t.CheckThrows(() => ok.Call(System.Array.Empty<object>()), typeof(LoxError), "enum constructor arity is enforced");

        // A field-shadowed callee must be a closure or a native, exactly
        // like vm.cpp's own INVOKE fast path - a class value stored in a
        // field is not callable here, even though LoxClass IS an
        // ILoxCallable.
        var box = new LoxClass("Box", null);
        var holder = new LoxClass("Holder", null);
        object h = holder.Call(System.Array.Empty<object>());
        LoxOps.SetProperty(h, "f", box);
        t.CheckThrows(() => LoxOps.Invoke(h, "f", System.Array.Empty<object>()), typeof(LoxError),
            "invoke() rejects a class stored in a shadowing field");

        // Codegen-facing guards for INHERIT and GET_TAG.
        t.CheckThrows(() => LoxOps.Inherit(1.0), typeof(LoxError), "inherit() rejects a non-class value");
        t.Check(ReferenceEquals(LoxOps.Inherit(animal), animal), "inherit() returns the validated superclass");
        t.CheckThrows(() => LoxOps.GetTag(1.0), typeof(LoxError), "getTag() rejects a non-enum value");
        t.CheckEquals(1.0, LoxOps.GetTag(a), "getTag() reads the constructor's tag as a Lox number");
        LoxOps.CheckMapKey("k"); // public for BUILD_MAP - must not throw for a valid key
        t.CheckThrows(() => LoxOps.CheckMapKey(double.NaN), typeof(LoxError), "checkMapKey() rejects NaN");

        return t.Finish("ClassesTest");
    }
}
