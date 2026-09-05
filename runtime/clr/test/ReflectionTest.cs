using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Lox;

namespace LoxRuntimeTests;

public static class ReflectionTest {
    private static object Call(LoxGlobals g, string name, params object[] args) {
        return ((ILoxCallable)g.Get(name)).Call(args);
    }

    public static int Run() {
        var t = new TestSupport();
        LoxGlobals globals = LoxRuntime.Init();

        CheckTypeLadder(t, globals);
        CheckFieldsAndMethods(t, globals);
        CheckFieldAccessors(t, globals);
        CheckCallMethod(t, globals);

        return t.Finish("ReflectionTest");
    }

    private static void CheckTypeLadder(TestSupport t, LoxGlobals globals) {
        t.CheckEquals("Number", Call(globals, "type", 1.0), "type(number)");
        t.CheckEquals("String", Call(globals, "type", "hi"), "type(string)");
        t.CheckEquals("Nil", Call(globals, "type", new object[] { null }), "type(nil)");
        t.CheckEquals("Boolean", Call(globals, "type", true), "type(boolean)");

        var animal = new LoxClass("Animal", null);
        var speak = new DelegateClosure("speak", 0, new object[0][], (self, a) => "...");
        LoxOps.DefineMethod(animal, "speak", speak);
        object rex = animal.Call(Array.Empty<object>());

        t.CheckEquals("Class", Call(globals, "type", animal), "type(class)");
        t.CheckEquals("Instance", Call(globals, "type", rex), "type(instance)");
        t.CheckEquals("Function", Call(globals, "type", speak), "type(user-defined closure)");

        object clockNative = globals.Get("clock");
        t.CheckEquals("Function", Call(globals, "type", clockNative), "type(plain native)");

        object boundMethod = LoxOps.GetProperty(rex, "speak");
        t.Check(boundMethod is LoxBoundMethod, "sanity: GET_PROPERTY on rex.speak gave a LoxBoundMethod");
        t.CheckEquals("BoundMethod", Call(globals, "type", boundMethod), "type(bound user-defined method)");

        var map = new LoxMap();
        map.Put(1.0, true);
        object mapMethod = map.GetMethod("has");
        t.Check(mapMethod is LoxMapMethod, "sanity: LoxMap.GetMethod gave a LoxMapMethod");
        t.CheckEquals("BoundMethod", Call(globals, "type", mapMethod), "type(bound map method)");
        t.CheckEquals("Map", Call(globals, "type", map), "type(map)");

        string filePath = Path.Combine(Path.GetTempPath(), $"lox-rt-reflection-type-{Guid.NewGuid():N}.txt");
        LoxFile file = LoxFile.Open(filePath, "w");
        try {
            object fileMethod = file.GetMethod("write");
            t.Check(fileMethod is LoxFileMethod, "sanity: LoxFile.GetMethod gave a LoxFileMethod");
            t.CheckEquals("BoundMethod", Call(globals, "type", fileMethod), "type(bound file method)");
            t.CheckEquals("File", Call(globals, "type", file), "type(file)");
        } finally {
            file.Close();
            File.Delete(filePath);
        }

        var list = new LoxList();
        list.Elements.Add(1.0);
        t.CheckEquals("List", Call(globals, "type", list), "type(list)");

        var iterator = new LoxIterator(list);
        t.CheckEquals("Iterator", Call(globals, "type", iterator), "type(iterator)");

        var ctor = new LoxEnumCtor(0, 1, "Ok", "Result");
        t.CheckEquals("EnumConstructor", Call(globals, "type", ctor), "type(enum constructor)");
        object enumVal = ctor.Call(new object[] { 5.0 });
        t.CheckEquals("Enum", Call(globals, "type", enumVal), "type(enum value)");
    }

    private static void CheckFieldsAndMethods(TestSupport t, LoxGlobals globals) {
        var animal = new LoxClass("Animal", null);
        var speak = new DelegateClosure("speak", 0, new object[0][], (self, a) => "...");
        LoxOps.DefineMethod(animal, "speak", speak);
        object rex = animal.Call(Array.Empty<object>());
        LoxOps.SetProperty(rex, "name", "Rex");
        LoxOps.SetProperty(rex, "age", 3.0);

        var fieldNames = ((LoxList)Call(globals, "fields", rex)).Elements.Cast<string>().ToHashSet();
        t.CheckEquals(true, fieldNames.SetEquals(new[] { "name", "age" }),
            "fields() lists every field, order-independent");

        var dog = new LoxClass("Dog", animal);
        var bark = new DelegateClosure("bark", 0, new object[0][], (self, a) => "Woof");
        LoxOps.DefineMethod(dog, "bark", bark);
        var methodNames = ((LoxList)Call(globals, "methods", dog)).Elements.Cast<string>().ToHashSet();
        t.CheckEquals(true, methodNames.SetEquals(new[] { "speak", "bark" }),
            "methods() includes methods inherited from a superclass");

        t.CheckThrows(() => Call(globals, "fields", 1.0), typeof(LoxError), "fields() rejects a non-instance");
        t.CheckThrows(() => Call(globals, "methods", 1.0), typeof(LoxError), "methods() rejects a non-class");
    }

    private static void CheckFieldAccessors(TestSupport t, LoxGlobals globals) {
        var animal = new LoxClass("Animal", null);
        var speak = new DelegateClosure("speak", 0, new object[0][], (self, a) => "...");
        LoxOps.DefineMethod(animal, "speak", speak);
        object rex = animal.Call(Array.Empty<object>());
        LoxOps.SetProperty(rex, "name", "Rex");

        t.CheckEquals("Rex", Call(globals, "getField", rex, "name"), "getField() reads an existing field");
        t.CheckEquals(null, Call(globals, "getField", rex, "missing"), "getField() returns nil for a missing field");
        t.CheckEquals(null, Call(globals, "getField", rex, "speak"),
            "getField() never falls back to a method, even one that exists");
        t.CheckEquals(true, Call(globals, "hasField", rex, "name"), "hasField() true for an existing field");
        t.CheckEquals(false, Call(globals, "hasField", rex, "missing"), "hasField() false for a missing field");
        t.CheckEquals(false, Call(globals, "hasField", rex, "speak"), "hasField() false for a method name");

        t.CheckEquals(7.0, Call(globals, "setField", rex, "age", 7.0), "setField() returns the assigned value");
        t.CheckEquals(7.0, Call(globals, "getField", rex, "age"), "setField() actually wrote the field");
        t.CheckEquals(1.0, Call(globals, "setField", rex, "brandNew", 1.0), "setField() creates an absent field");
        t.CheckEquals(true, Call(globals, "hasField", rex, "brandNew"), "the newly-created field now shows up");

        t.CheckThrows(() => Call(globals, "getField", 1.0, "x"), typeof(LoxError), "getField() rejects a non-instance");
        t.CheckThrows(() => Call(globals, "hasField", 1.0, "x"), typeof(LoxError), "hasField() rejects a non-instance");
        t.CheckThrows(() => Call(globals, "setField", 1.0, "x", 1.0), typeof(LoxError),
            "setField() rejects a non-instance");
        t.CheckThrows(() => Call(globals, "getField", rex, 1.0), typeof(LoxError),
            "getField() rejects a non-string name");
    }

    private static void CheckCallMethod(TestSupport t, LoxGlobals globals) {
        // callMethod() on a plain native stored in a field.
        var foo = new LoxClass("Foo", null);
        object f = foo.Call(Array.Empty<object>());
        LoxOps.SetProperty(f, "describe", globals.Get("str"));
        t.CheckEquals("42", Call(globals, "callMethod", f, "describe", 42.0),
            "callMethod() calls a native function stored in a field");
        t.CheckThrows(() => Call(globals, "callMethod", f, "describe"), typeof(LoxError),
            "callMethod() enforces the resolved native's own arity");

        // callMethod() on the bound Map representation - confirm the
        // receiver captured in the closure is the map it was read from,
        // not callMethod's own `instance` argument.
        var m = new LoxMap();
        m.Put(1.0, true);
        LoxOps.SetProperty(f, "h", m.GetMethod("has"));
        t.CheckEquals(true, Call(globals, "callMethod", f, "h", 1.0),
            "callMethod() on a bound map method finds a key that is present");
        t.CheckEquals(false, Call(globals, "callMethod", f, "h", 2.0),
            "callMethod() on a bound map method correctly reports a key that is absent");

        // callMethod() on the bound File representation - same receiver check.
        string path = Path.Combine(Path.GetTempPath(), $"lox-rt-reflection-callmethod-{Guid.NewGuid():N}.txt");
        try {
            LoxFile writer = LoxFile.Open(path, "w");
            LoxOps.SetProperty(f, "w", writer.GetMethod("write"));
            Call(globals, "callMethod", f, "w", "hello");
            writer.Close();

            LoxFile reader = LoxFile.Open(path, "r");
            t.CheckEquals("hello", reader.Read(),
                "callMethod() on a bound file method wrote through the correct receiver");
            reader.Close();
        } finally {
            File.Delete(path);
        }

        // callMethod() must reject a user-defined (closure-backed) method,
        // whether resolved via the class's method table or via a field.
        var bar = new LoxClass("Bar", null);
        var greet = new DelegateClosure("greet", 0, new object[0][], (self, a) => "hi");
        LoxOps.DefineMethod(bar, "greet", greet);
        object b = bar.Call(Array.Empty<object>());
        t.CheckThrows(() => Call(globals, "callMethod", b, "greet"), typeof(LoxError),
            "callMethod() rejects a method resolved from the class's method table");

        LoxOps.SetProperty(b, "closureField", greet);
        t.CheckThrows(() => Call(globals, "callMethod", b, "closureField"), typeof(LoxError),
            "callMethod() rejects a closure resolved from a field too");

        t.CheckThrows(() => Call(globals, "callMethod", b, "missingName"), typeof(LoxError),
            "callMethod() rejects an undefined name");
        t.CheckThrows(() => Call(globals, "callMethod", 1.0, "x"), typeof(LoxError),
            "callMethod() rejects a non-instance receiver");
        t.CheckThrows(() => Call(globals, "callMethod", b), typeof(LoxError),
            "callMethod() requires at least 2 arguments");
    }
}
