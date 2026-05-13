// test_reflect.cpp — reflect API runtime tests.
//
// Invariants under test:
//   1.  typeOf — correct string for each value kind
//   2.  isXxx predicates — true/false for matching/non-matching types
//   3.  classOf — returns the class of an instance; errors on non-instance
//   4.  className — returns name string of a class; errors on non-class
//   5.  superOf — returns superclass or nil; errors on non-class
//   6.  fields — lists own instance field names; errors on non-instance
//   7.  methods — lists all class method names (incl. inherited); errors on
//   non-class
//   8.  hasField — bool lookup; errors on wrong types
//   9.  getField — returns value; errors on missing field or wrong types
//  10.  setField — writes field; readable back via direct access
//  11.  hasMethod — bool lookup on class; errors on wrong types
//  12.  getMethod — returns callable bound method; errors on missing method
//  13.  GC safety — fields/methods under stress GC

#include "test_harness.h"
#include <gtest/gtest.h>

class ReflectTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// 1. typeOf
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, TypeOfPrimitives) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var a = reflect.typeOf(nil);
        var b = reflect.typeOf(true);
        var c = reflect.typeOf(42);
        var d = reflect.typeOf("hello");
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("a"), "nil");
    EXPECT_EQ(h.getGlobalStr("b"), "bool");
    EXPECT_EQ(h.getGlobalStr("c"), "number");
    EXPECT_EQ(h.getGlobalStr("d"), "string");
}

TEST_F(ReflectTest, TypeOfObjects) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Foo {}
        var inst = Foo();
        var tInst = reflect.typeOf(inst);
        var tCls  = reflect.typeOf(Foo);
        fun bar() {}
        var tFun  = reflect.typeOf(bar);
        var tList = reflect.typeOf([1, 2]);
        var tMap  = reflect.typeOf({});
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("tInst"), "instance");
    EXPECT_EQ(h.getGlobalStr("tCls"), "class");
    EXPECT_EQ(h.getGlobalStr("tFun"), "function");
    EXPECT_EQ(h.getGlobalStr("tList"), "list");
    EXPECT_EQ(h.getGlobalStr("tMap"), "map");
}

// ---------------------------------------------------------------------------
// 2. isXxx predicates
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, IsPredicatesMatchCorrectType) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var n  = reflect.isNil(nil);
        var b  = reflect.isBool(false);
        var nm = reflect.isNumber(3.14);
        var s  = reflect.isString("x");
        var l  = reflect.isList([]);
        var m  = reflect.isMap({});
        class C {}
        var c  = reflect.isClass(C);
        var i  = reflect.isInstance(C());
        fun f() {}
        var fn = reflect.isFunction(f);
    )"),
              InterpretResult::OK);
    EXPECT_TRUE(as<bool>(h.getGlobal("n").value()));
    EXPECT_TRUE(as<bool>(h.getGlobal("b").value()));
    EXPECT_TRUE(as<bool>(h.getGlobal("nm").value()));
    EXPECT_TRUE(as<bool>(h.getGlobal("s").value()));
    EXPECT_TRUE(as<bool>(h.getGlobal("l").value()));
    EXPECT_TRUE(as<bool>(h.getGlobal("m").value()));
    EXPECT_TRUE(as<bool>(h.getGlobal("c").value()));
    EXPECT_TRUE(as<bool>(h.getGlobal("i").value()));
    EXPECT_TRUE(as<bool>(h.getGlobal("fn").value()));
}

TEST_F(ReflectTest, IsPredicatesRejectWrongType) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        var a = reflect.isNil(0);
        var b = reflect.isBool(nil);
        var c = reflect.isNumber("3");
        var d = reflect.isString(42);
        var e = reflect.isList({});
        var f = reflect.isMap([]);
        class C {}
        var g = reflect.isClass(C());
        var h = reflect.isInstance(C);
    )"),
              InterpretResult::OK);
    EXPECT_FALSE(as<bool>(h.getGlobal("a").value()));
    EXPECT_FALSE(as<bool>(h.getGlobal("b").value()));
    EXPECT_FALSE(as<bool>(h.getGlobal("c").value()));
    EXPECT_FALSE(as<bool>(h.getGlobal("d").value()));
    EXPECT_FALSE(as<bool>(h.getGlobal("e").value()));
    EXPECT_FALSE(as<bool>(h.getGlobal("f").value()));
    EXPECT_FALSE(as<bool>(h.getGlobal("g").value()));
    EXPECT_FALSE(as<bool>(h.getGlobal("h").value()));
}

TEST_F(ReflectTest, IsFunctionCoversCallables) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class C {
            greet() { return "hi"; }
        }
        var inst = C();
        var m = reflect.getMethod(inst, "greet");
        var fCls = reflect.isFunction(C);
        var fBound = reflect.isFunction(m);
        var fNotFn = reflect.isFunction(42);
    )"),
              InterpretResult::OK);
    EXPECT_TRUE(as<bool>(h.getGlobal("fCls").value()));
    EXPECT_TRUE(as<bool>(h.getGlobal("fBound").value()));
    EXPECT_FALSE(as<bool>(h.getGlobal("fNotFn").value()));
}

// ---------------------------------------------------------------------------
// 3. classOf
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, ClassOfReturnsClass) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Dog {}
        var d = Dog();
        var name = reflect.className(reflect.classOf(d));
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("name"), "Dog");
}

TEST_F(ReflectTest, ClassOfErrorOnNonInstance) {
    VMTestHarness h;
    EXPECT_EQ(h.run("reflect.classOf(42);"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// 4. className
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, ClassNameReturnsString) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class MyClass {}
        var n = reflect.className(MyClass);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("n"), "MyClass");
}

TEST_F(ReflectTest, ClassNameErrorOnNonClass) {
    VMTestHarness h;
    EXPECT_EQ(h.run("reflect.className(\"Dog\");"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// 5. superOf
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, SuperOfReturnsParentClass) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Animal {}
        class Dog < Animal {}
        var sup = reflect.superOf(Dog);
        var name = reflect.className(sup);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("name"), "Animal");
}

TEST_F(ReflectTest, SuperOfNilWhenNoSuperclass) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Base {}
        var sup = reflect.superOf(Base);
        var isNilResult = reflect.isNil(sup);
    )"),
              InterpretResult::OK);
    EXPECT_TRUE(as<bool>(h.getGlobal("isNilResult").value()));
}

TEST_F(ReflectTest, SuperOfChain) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class A {}
        class B < A {}
        class C < B {}
        var s1 = reflect.className(reflect.superOf(C));
        var s2 = reflect.className(reflect.superOf(reflect.superOf(C)));
        var s3 = reflect.isNil(reflect.superOf(reflect.superOf(reflect.superOf(C))));
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("s1"), "B");
    EXPECT_EQ(h.getGlobalStr("s2"), "A");
    EXPECT_TRUE(as<bool>(h.getGlobal("s3").value()));
}

TEST_F(ReflectTest, SuperOfErrorOnNonClass) {
    VMTestHarness h;
    EXPECT_EQ(h.run("reflect.superOf(nil);"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// 6. fields
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, FieldsListsOwnFields) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Point {
            init(x, y) { this.x = x; this.y = y; }
        }
        var p = Point(1, 2);
        var fs = reflect.fields(p);
        var count = len(fs);
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(h.getGlobal("count").value()), 2.0);
}

TEST_F(ReflectTest, FieldsEmptyOnFreshInstance) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Empty {}
        var e = Empty();
        var fs = reflect.fields(e);
        var count = len(fs);
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(h.getGlobal("count").value()), 0.0);
}

TEST_F(ReflectTest, FieldsErrorOnNonInstance) {
    VMTestHarness h;
    EXPECT_EQ(h.run("reflect.fields(42);"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// 7. methods
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, MethodsListsOwnMethods) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Foo {
            init() {}
            bar() {}
        }
        var ms = reflect.methods(Foo);
        var count = len(ms);
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(h.getGlobal("count").value()), 2.0);
}

TEST_F(ReflectTest, MethodsIncludesInherited) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Base {
            baseMethod() {}
        }
        class Child < Base {
            childMethod() {}
        }
        var ms = reflect.methods(Child);
        var count = len(ms);
    )"),
              InterpretResult::OK);
    // Child's methods table has both baseMethod (copied by INHERIT) and
    // childMethod
    EXPECT_DOUBLE_EQ(as<Number>(h.getGlobal("count").value()), 2.0);
}

TEST_F(ReflectTest, MethodsErrorOnNonClass) {
    VMTestHarness h;
    EXPECT_EQ(h.run("reflect.methods(42);"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// 8. hasField
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, HasFieldTrueForExistingField) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box { init(v) { this.val = v; } }
        var b = Box(1);
        var result = reflect.hasField(b, "val");
    )"),
              InterpretResult::OK);
    EXPECT_TRUE(as<bool>(h.getGlobal("result").value()));
}

TEST_F(ReflectTest, HasFieldFalseForMissingField) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box {}
        var b = Box();
        var result = reflect.hasField(b, "missing");
    )"),
              InterpretResult::OK);
    EXPECT_FALSE(as<bool>(h.getGlobal("result").value()));
}

TEST_F(ReflectTest, HasFieldErrorOnNonInstance) {
    VMTestHarness h;
    EXPECT_EQ(h.run("reflect.hasField(42, \"x\");"),
              InterpretResult::RUNTIME_ERROR);
}

TEST_F(ReflectTest, HasFieldErrorOnNonStringName) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box {}
        var b = Box();
    )"),
              InterpretResult::OK);
    VMTestHarness h2;
    EXPECT_EQ(h2.run(R"(
        class Box {}
        var b = Box();
        reflect.hasField(b, 42);
    )"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// 9. getField
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, GetFieldReturnsValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box { init(v) { this.val = v; } }
        var b = Box("hello");
        var result = reflect.getField(b, "val");
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "hello");
}

TEST_F(ReflectTest, GetFieldErrorOnMissingField) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(
        class Box {}
        var b = Box();
        reflect.getField(b, "missing");
    )"),
              InterpretResult::RUNTIME_ERROR);
}

TEST_F(ReflectTest, GetFieldErrorOnNonInstance) {
    VMTestHarness h;
    EXPECT_EQ(h.run("reflect.getField(nil, \"x\");"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// 10. setField
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, SetFieldWritesNewField) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box {}
        var b = Box();
        reflect.setField(b, "x", 99);
        var result = b.x;
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(h.getGlobal("result").value()), 99.0);
}

TEST_F(ReflectTest, SetFieldUpdatesExistingField) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Box { init(v) { this.val = v; } }
        var b = Box("old");
        reflect.setField(b, "val", "new");
        var result = b.val;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "new");
}

TEST_F(ReflectTest, SetFieldErrorOnNonInstance) {
    VMTestHarness h;
    EXPECT_EQ(h.run("reflect.setField(42, \"x\", 1);"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// 11. hasMethod
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, HasMethodTrueForExistingMethod) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Greeter { greet() {} }
        var result = reflect.hasMethod(Greeter, "greet");
    )"),
              InterpretResult::OK);
    EXPECT_TRUE(as<bool>(h.getGlobal("result").value()));
}

TEST_F(ReflectTest, HasMethodFalseForMissingMethod) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Greeter {}
        var result = reflect.hasMethod(Greeter, "missing");
    )"),
              InterpretResult::OK);
    EXPECT_FALSE(as<bool>(h.getGlobal("result").value()));
}

TEST_F(ReflectTest, HasMethodErrorOnNonClass) {
    VMTestHarness h;
    EXPECT_EQ(h.run("reflect.hasMethod(42, \"foo\");"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// 12. getMethod
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, GetMethodReturnsBoundMethodAndCallable) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Counter {
            init() { this.n = 0; }
            inc() { this.n = this.n + 1; return this.n; }
        }
        var c = Counter();
        var m = reflect.getMethod(c, "inc");
        var r1 = m();
        var r2 = m();
    )"),
              InterpretResult::OK);
    EXPECT_DOUBLE_EQ(as<Number>(h.getGlobal("r1").value()), 1.0);
    EXPECT_DOUBLE_EQ(as<Number>(h.getGlobal("r2").value()), 2.0);
}

TEST_F(ReflectTest, GetMethodBindsReceiverCorrectly) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Dog {
            init(name) { this.name = name; }
            speak() { return this.name + " barks"; }
        }
        var d = Dog("Rex");
        var m = reflect.getMethod(d, "speak");
        var result = m();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "Rex barks");
}

TEST_F(ReflectTest, GetMethodInheritedMethod) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Animal {
            init(name) { this.name = name; }
            speak() { return this.name + "!"; }
        }
        class Cat < Animal {}
        var c = Cat("Whiskers");
        var m = reflect.getMethod(c, "speak");
        var result = m();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "Whiskers!");
}

TEST_F(ReflectTest, GetMethodErrorOnMissingMethod) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(
        class Box {}
        var b = Box();
        reflect.getMethod(b, "missing");
    )"),
              InterpretResult::RUNTIME_ERROR);
}

TEST_F(ReflectTest, GetMethodErrorOnNonInstance) {
    VMTestHarness h;
    EXPECT_EQ(h.run("reflect.getMethod(42, \"foo\");"),
              InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// 13. GC safety — fields/methods allocation under GC stress
// ---------------------------------------------------------------------------

TEST_F(ReflectTest, FieldsMethodsGCSafe) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Node {
            init(v) { this.val = v; this.next = nil; }
            getValue() { return this.val; }
        }
        var i = 0;
        while (i < 200) {
            var n = Node(i);
            var fs = reflect.fields(n);
            var ms = reflect.methods(Node);
            i = i + 1;
        }
        var ok = true;
    )"),
              InterpretResult::OK);
    EXPECT_TRUE(as<bool>(h.getGlobal("ok").value()));
}
