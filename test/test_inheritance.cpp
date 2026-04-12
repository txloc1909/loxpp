// test_inheritance.cpp — single-inheritance runtime tests.
//
// Invariants under test:
//   1. Method inheritance   — subclass can call an inherited method.
//   2. Method override      — subclass version shadows the superclass version.
//   3. super.method()       — calls superclass method from subclass body.
//   4. Inherited init       — subclass with no init uses superclass init.
//   5. super.init()         — subclass init delegates to superclass init.
//   6. super non-call       — super.method stored in var, then called.
//   7. Multi-level          — grandparent->parent->child, super resolves one level.
//   8. Compile error: super outside class.
//   9. Compile error: super in class without superclass.
//  10. Compile error: self-inheritance.
//  11. Runtime error: superclass is not a class.

#include "test_harness.h"
#include <gtest/gtest.h>

class InheritanceTest : public ::testing::Test {};

// ---------------------------------------------------------------------------
// 1. Method inheritance: subclass inherits a method from superclass.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, InheritedMethodIsCallable) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Animal {
            speak() { return "generic sound"; }
        }
        class Dog < Animal {}
        var d = Dog();
        var result = d.speak();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "generic sound");
}

// ---------------------------------------------------------------------------
// 2. Method override: subclass version is called, not superclass.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, OverriddenMethodIsCalled) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Animal {
            speak() { return "generic"; }
        }
        class Cat < Animal {
            speak() { return "meow"; }
        }
        var c = Cat();
        var result = c.speak();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "meow");
}

// ---------------------------------------------------------------------------
// 3. super.method() — SUPER_INVOKE path.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, SuperMethodCall) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Base {
            greet() { return "hello from Base"; }
        }
        class Child < Base {
            greet() { return super.greet(); }
        }
        var c = Child();
        var result = c.greet();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "hello from Base");
}

// ---------------------------------------------------------------------------
// 4. Inherited init: subclass with no init uses superclass init.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, InheritedInit) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Animal {
            init(name) { this.name = name; }
        }
        class Dog < Animal {}
        var d = Dog("Rex");
        var result = d.name;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "Rex");
}

// ---------------------------------------------------------------------------
// 5. super.init() — subclass init delegates to superclass init.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, SuperInitCall) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Animal {
            init(name) { this.name = name; }
        }
        class Dog < Animal {
            init(name, breed) {
                super.init(name);
                this.breed = breed;
            }
        }
        var d = Dog("Rex", "Husky");
        var name = d.name;
        var breed = d.breed;
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("name"), "Rex");
    EXPECT_EQ(h.getGlobalStr("breed"), "Husky");
}

// ---------------------------------------------------------------------------
// 6. super non-call: GET_SUPER path — store bound method in variable.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, SuperNonCall_BoundMethod) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class Base {
            greet() { return "hi"; }
        }
        class Child < Base {
            getSuper() { return super.greet; }
        }
        var c = Child();
        var m = c.getSuper();
        var result = m();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "hi");
}

// ---------------------------------------------------------------------------
// 7. Multi-level inheritance: grandparent -> parent -> child.
//    super in child resolves to parent, not grandparent.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, MultiLevelInheritance) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(
        class A {
            who() { return "A"; }
        }
        class B < A {
            who() { return "B"; }
        }
        class C < B {
            who() { return super.who(); }
        }
        var c = C();
        var result = c.who();
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "B");
}

// ---------------------------------------------------------------------------
// 8. Compile error: 'super' outside of any class.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, CompileError_SuperOutsideClass) {
    EXPECT_EQ(run_program("super.foo();"), InterpretResult::COMPILE_ERROR);
}

// ---------------------------------------------------------------------------
// 9. Compile error: 'super' in a class with no superclass.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, CompileError_SuperInClassWithNoSuperclass) {
    EXPECT_EQ(run_program(R"(
        class Foo {
            bar() { return super.baz(); }
        }
    )"),
              InterpretResult::COMPILE_ERROR);
}

// ---------------------------------------------------------------------------
// 10. Compile error: self-inheritance.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, CompileError_SelfInheritance) {
    EXPECT_EQ(run_program("class Foo < Foo {}"), InterpretResult::COMPILE_ERROR);
}

// ---------------------------------------------------------------------------
// 11. Runtime error: superclass is not a class.
// ---------------------------------------------------------------------------

TEST_F(InheritanceTest, RuntimeError_SuperclassNotAClass) {
    EXPECT_EQ(run_program(R"(
        var notAClass = "oops";
        class Foo < notAClass {}
    )"),
              InterpretResult::RUNTIME_ERROR);
}
