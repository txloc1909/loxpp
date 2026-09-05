// test_reflect_api.cpp — the reflection / introspection standard-library
// natives.
//
// Invariants under test:
//   1. type()       — reports the language-level type name of any value.
//   2. fields()      / methods()  — enumerate an instance's fields / a
//      class's methods (fields()/methods() are hash-order; tests sort).
//   3. getField()/hasField()/setField() — fields-only property access (no
//      method fallback, unlike the `.` operator).
//   4. callMethod()  — dynamic dispatch by name, capped to natives-only in
//      v1 (see notes/expressiveness-roadmap.md item 1).

#include "test_harness.h"
#include "class_objects.h"
#include "container_objects.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

// Reads an ObjList of Strings out of a Value and returns their contents,
// sorted — fields()/methods() order is unspecified (hash-table order), so
// every test compares as a sorted set.
static std::vector<std::string> sortedStringListContents(const Value& v) {
    ObjList* list = asObjList(as<Obj*>(v));
    std::vector<std::string> out;
    for (const Value& elem : list->elements) {
        ObjString* s = asObjString(as<Obj*>(elem));
        out.emplace_back(s->chars.data(), s->chars.size());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// type()
// ---------------------------------------------------------------------------

TEST(ReflectApi, Type_Nil) {
    VMTestHarness h;
    ASSERT_EQ(h.run("type(nil);"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "Nil");
}

TEST(ReflectApi, Type_Boolean) {
    VMTestHarness h;
    ASSERT_EQ(h.run("type(true);"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "Boolean");
}

TEST(ReflectApi, Type_Number) {
    VMTestHarness h;
    ASSERT_EQ(h.run("type(1);"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "Number");
}

TEST(ReflectApi, Type_String) {
    VMTestHarness h;
    ASSERT_EQ(h.run(R"(type("hi");)"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "String");
}

TEST(ReflectApi, Type_ClosureIsFunction) {
    VMTestHarness h;
    ASSERT_EQ(h.run("fun f() {} type(f);"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "Function");
}

TEST(ReflectApi, Type_NativeIsFunction) {
    VMTestHarness h;
    ASSERT_EQ(h.run("type(clock);"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "Function");
}

TEST(ReflectApi, Type_BoundMethod) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo { greet() { return 1; } } "
                    "var f = Foo(); type(f.greet);"),
              InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "BoundMethod");
}

TEST(ReflectApi, Type_BoundNativeIsBoundMethod) {
    VMTestHarness h;
    ASSERT_EQ(h.run("var m = {}; type(m.has);"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "BoundMethod");
}

TEST(ReflectApi, Type_Class) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} type(Foo);"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "Class");
}

TEST(ReflectApi, Type_Instance) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} type(Foo());"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "Instance");
}

TEST(ReflectApi, Type_List) {
    VMTestHarness h;
    ASSERT_EQ(h.run("type([1, 2]);"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "List");
}

TEST(ReflectApi, Type_Map) {
    VMTestHarness h;
    ASSERT_EQ(h.run("type({});"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "Map");
}

TEST(ReflectApi, Type_Enum) {
    VMTestHarness h;
    ASSERT_EQ(h.run("enum Msg { Ping } type(Ping());"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "Enum");
}

TEST(ReflectApi, Type_EnumConstructor) {
    VMTestHarness h;
    ASSERT_EQ(h.run("enum Opt { Some(x) } type(Some);"), InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "EnumConstructor");
}

// File is exercised via a fixture since it needs a real path.
class ReflectApiFileTest : public ::testing::Test {
  protected:
    void SetUp() override {
        std::error_code ec;
        std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        ASSERT_FALSE(ec);
        m_file =
            base / ("loxpp_reflect_api_" + std::to_string(::getpid()) + ".txt");
        std::ofstream out(m_file);
        out << "x";
        ASSERT_TRUE(out.good());
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(m_file, ec);
    }
    std::filesystem::path m_file;
};

TEST_F(ReflectApiFileTest, Type_File) {
    VMTestHarness h;
    ASSERT_EQ(
        h.run("var f = open(\"" + m_file.string() + "\", \"r\"); type(f);"),
        InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "File");
}

// ---------------------------------------------------------------------------
// fields() / methods()
// ---------------------------------------------------------------------------

TEST(ReflectApi, Fields_ReturnsFieldNames) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} var f = Foo(); f.a = 1; f.b = 2; "
                    "fields(f);"),
              InterpretResult::OK);
    std::vector<std::string> expected = {"a", "b"};
    EXPECT_EQ(sortedStringListContents(h.lastResult()), expected);
}

TEST(ReflectApi, Fields_NonInstance_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("fields(1);"), InterpretResult::RUNTIME_ERROR);
}

TEST(ReflectApi, Methods_ReturnsInheritedAndOwnMethods) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Animal { speak() { return 1; } } "
                    "class Dog < Animal { bark() { return 2; } } "
                    "methods(Dog);"),
              InterpretResult::OK);
    std::vector<std::string> expected = {"bark", "speak"};
    EXPECT_EQ(sortedStringListContents(h.lastResult()), expected);
}

TEST(ReflectApi, Methods_NonClass_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("methods(1);"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// getField() / hasField() / setField()
// ---------------------------------------------------------------------------

TEST(ReflectApi, GetField_PresentField_ReturnsValue) {
    VMTestHarness h;
    ASSERT_EQ(
        h.run("class Foo {} var f = Foo(); f.a = 42; getField(f, \"a\");"),
        InterpretResult::OK);
    EXPECT_EQ(as<Number>(h.lastResult()), 42.0);
}

TEST(ReflectApi, GetField_AbsentField_ReturnsNil) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} var f = Foo(); getField(f, \"missing\");"),
              InterpretResult::OK);
    EXPECT_TRUE(is<Nil>(h.lastResult()));
}

TEST(ReflectApi, GetField_NonInstance_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(getField(1, "a");)"), InterpretResult::RUNTIME_ERROR);
}

TEST(ReflectApi, GetField_DoesNotFallBackToMethod) {
    // Deliberate deviation from `.` property access: a method name is not a
    // field, even though `f.greet` (the `.` operator) would find it.
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo { greet() { return 1; } } var f = Foo(); "
                    "getField(f, \"greet\");"),
              InterpretResult::OK);
    EXPECT_TRUE(is<Nil>(h.lastResult()));
}

TEST(ReflectApi, HasField_PresentField_True) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} var f = Foo(); f.a = 1; hasField(f, \"a\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);
}

TEST(ReflectApi, HasField_AbsentField_False) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} var f = Foo(); hasField(f, \"a\");"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), false);
}

TEST(ReflectApi, HasField_NonInstance_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(hasField(1, "a");)"), InterpretResult::RUNTIME_ERROR);
}

TEST(ReflectApi, SetField_CreatesNewField) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} var f = Foo(); setField(f, \"a\", 5); "
                    "f.a;"),
              InterpretResult::OK);
    EXPECT_EQ(as<Number>(h.lastResult()), 5.0);
}

TEST(ReflectApi, SetField_OverwritesExisting) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} var f = Foo(); f.a = 1; "
                    "setField(f, \"a\", 2); f.a;"),
              InterpretResult::OK);
    EXPECT_EQ(as<Number>(h.lastResult()), 2.0);
}

TEST(ReflectApi, SetField_ReturnsAssignedValue) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} var f = Foo(); setField(f, \"a\", 7);"),
              InterpretResult::OK);
    EXPECT_EQ(as<Number>(h.lastResult()), 7.0);
}

TEST(ReflectApi, SetField_NonInstance_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(setField(1, "a", 2);)"), InterpretResult::RUNTIME_ERROR);
}

// ---------------------------------------------------------------------------
// callMethod()
// ---------------------------------------------------------------------------

TEST(ReflectApi, CallMethod_NativeFieldValue_ForwardsArgs) {
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} var f = Foo(); f.s = str; "
                    "callMethod(f, \"s\", 42);"),
              InterpretResult::OK);
    EXPECT_EQ(asObjString(as<Obj*>(h.lastResult()))->chars, "42");
}

TEST(ReflectApi, CallMethod_BoundNativeFieldValue_ForwardsReceiverAndArgs) {
    // f.has is a bound-native (Map's `has` bound to `m`) stored in a field —
    // this is the one path that needs the receiver-shuffle in argv[1].
    VMTestHarness h;
    ASSERT_EQ(h.run("class Foo {} var f = Foo(); var m = {1: true}; "
                    "f.h = m.has; callMethod(f, \"h\", 1);"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h.lastResult()), true);

    VMTestHarness h2;
    ASSERT_EQ(h2.run("class Foo {} var f = Foo(); var m = {1: true}; "
                     "f.h = m.has; callMethod(f, \"h\", 2);"),
              InterpretResult::OK);
    EXPECT_EQ(as<bool>(h2.lastResult()), false);
}

TEST(ReflectApi, CallMethod_ClosureBackedMethod_RuntimeError) {
    // The v1 restriction: this is exactly the check that must be shown
    // failing correctly (AGENTS.md: "prove that a new check can fail").
    VMTestHarness h;
    EXPECT_EQ(h.run("class Foo { greet() { return 1; } } var f = Foo(); "
                    "callMethod(f, \"greet\");"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(ReflectApi, CallMethod_UndefinedMethod_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("class Foo {} var f = Foo(); callMethod(f, \"missing\");"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(ReflectApi, CallMethod_WrongArityToResolvedNative_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("class Foo {} var f = Foo(); f.s = str; "
                    "callMethod(f, \"s\");"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(ReflectApi, CallMethod_TooFewArguments_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run("class Foo {} var f = Foo(); callMethod(f);"),
              InterpretResult::RUNTIME_ERROR);
}

TEST(ReflectApi, CallMethod_NonInstance_RuntimeError) {
    VMTestHarness h;
    EXPECT_EQ(h.run(R"(callMethod(1, "a");)"), InterpretResult::RUNTIME_ERROR);
}
