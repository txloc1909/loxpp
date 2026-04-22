// test_gc_regression.cpp
//
// Regression tests for GC use-after-free bugs #31 and #32.
// Compiled unconditionally with LOXPP_STRESS_GC so GC fires on every
// allocation. Run under AddressSanitizer (cmake --preset debug) for reliable
// detection; without ASAN the UAF is silent UB that may or may not corrupt
// visible state depending on allocator reuse behaviour.

#include "test_harness.h"
#include <gtest/gtest.h>

// ---------------------------------------------------------------------------
// Bug #31: ObjNative unrooted in VM::defineNative (vm.cpp:430)
//
// Timeline under LOXPP_STRESS_GC:
//   interpret() calls compile() — bytesAllocated >> 0 after compilation.
//   m_markRoots callback is NOT yet set (registered only just before run(),
//   vm.cpp:62).  m_currentCompiler is nullptr (Compiler was destroyed when
//   compile() returned).
//
//   defineNative("clock", ...)
//     create<ObjNative>  →  bytesAllocated > m_nextGC(=0)  →  GC fires:
//       m_markRoots == nullptr && m_currentCompiler == nullptr
//       → nothing marked → entire allObjects heap swept
//     ObjNative allocated, added to allObjects
//     makeString("clock")  →  create<ObjString>  →  GC fires again:
//       ObjNative in allObjects, not reachable from any root  →  swept/freed
//     m_globals.set(key, Value{dangling_native})  →  UAF written to globals
//
// ASAN detects: heap-use-after-free in sweep() while processing the ObjNative
// entry, with the allocation site in defineNative and the free site in sweep.
// Without ASAN: callNative dereferences native->function from freed memory;
// happens to work when the allocator has not yet reused the block.
// ---------------------------------------------------------------------------

TEST(GcRegression, Bug31_NativeUnrootedInDefineNative) {
    VMTestHarness h;
    // str() is a native function registered by defineNatives() inside
    // interpret().  Under ASAN+STRESS_GC this test aborts during VM
    // initialisation — before the Lox code even begins executing — with a
    // heap-use-after-free pointing into VM::defineNative / sweep().
    ASSERT_EQ(h.run(R"(
        var result = str(123);
    )"),
              InterpretResult::OK);
    EXPECT_EQ(h.getGlobalStr("result"), "123");
}

// ---------------------------------------------------------------------------
// Bug #32: ObjFunction unrooted in Compiler::funDeclaration for local
// functions (compiler.cpp:469-470)
//
// Timeline under LOXPP_STRESS_GC (m_scopeDepth > 0 path only):
//   funDeclaration() for a local function:
//     create<ObjFunction>()
//       → fn allocated, added to allObjects
//       → m_currentCompiler == outer Compiler (correct)
//         outer Compiler's markRoots marks only outer->m_function
//         fn is NOT outer->m_function and NOT yet in outer's constant pool
//     makeString(name.lexeme)  →  create<ObjString>  →  GC fires:
//       fn is in allObjects, not reachable from any root  →  swept/freed
//     fn->name = <result>  →  write to freed ObjFunction  →  UAF
//     Compiler inner(fn, ...)  →  inner.m_function = dangling pointer
//
// Global functions are NOT affected: identifierConstant() interns the name
// string before create<ObjFunction>(), so the subsequent makeString() finds
// it in the intern table and returns without allocating — no GC triggered.
//
// ASAN detects: heap-use-after-free on the fn->name assignment line inside
// funDeclaration, with the allocation in create<ObjFunction> and the free
// in sweep().
// ---------------------------------------------------------------------------

TEST(GcRegression, Bug32_ObjFunctionUnrootedInLocalFunDeclaration) {
    VMTestHarness h;
    // Declaring 'inner' inside 'outer' forces funDeclaration to execute with
    // m_scopeDepth > 0, hitting the unrooted-ObjFunction code path.
    // Under ASAN+STRESS_GC this test aborts during compilation of the source
    // with a heap-use-after-free pointing into funDeclaration / sweep().
    ASSERT_EQ(h.run(R"(
        fun outer() {
            fun inner(x) { return x + 1; }
            return inner(41);
        }
        var result = outer();
    )"),
              InterpretResult::OK);
    auto v = h.getGlobal("result");
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(as<Number>(*v), 42.0);
}
