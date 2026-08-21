using Lox;

namespace LoxRuntimeTests;

/// <summary>LoxOps.Call (the CALL opcode's runtime dispatch) and LoxRuntime.Current().</summary>
public static class CallDispatchTest {
    public static int Run() {
        var t = new TestSupport();

        var add = new DelegateClosure("add", 2, new object[0][], (self, a) => LoxOps.Add(a[0], a[1]));
        t.CheckEquals(3.0, LoxOps.Call(add, new object[] { 1.0, 2.0 }), "call() dispatches to a closure");
        t.CheckThrows(() => LoxOps.Call(add, new object[] { 1.0 }), typeof(LoxError),
            "call() still enforces the callee's own arity check");

        var clock = new LoxNative("clock", 0, a => 42.0);
        t.CheckEquals(42.0, LoxOps.Call(clock, System.Array.Empty<object>()), "call() dispatches to a native");

        var box = new LoxClass("Box", null);
        object instance = LoxOps.Call(box, System.Array.Empty<object>());
        t.Check(instance is LoxInstance, "call() dispatches to a class (construction)");

        var ok = new LoxEnumCtor(1, 1, "Ok", "Result");
        object enumVal = LoxOps.Call(ok, new object[] { 5.0 });
        t.Check(enumVal is LoxEnum, "call() dispatches to an enum constructor");

        var bound = new LoxBoundMethod(instance, add);
        t.CheckEquals(3.0, LoxOps.Call(bound, new object[] { 1.0, 2.0 }), "call() dispatches to a bound method");

        t.CheckThrows(() => LoxOps.Call(1.0, System.Array.Empty<object>()), typeof(LoxError),
            "call() rejects a value that is not callable");
        t.CheckThrows(() => LoxOps.Call(null, System.Array.Empty<object>()), typeof(LoxError),
            "call() rejects nil the same way as any other non-callable value");

        LoxGlobals g = LoxRuntime.Init();
        t.Check(ReferenceEquals(LoxRuntime.Current(), g), "current() returns the instance init() just built");

        return t.Finish("CallDispatchTest");
    }
}
