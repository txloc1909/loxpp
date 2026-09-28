// test_backend_qbe_emitter.cpp — QBE straight-line + jumps emitter (S3,
// issue #456, notes/qbe-backend.md).
//
// Checkpoint: probes 01-05 and 15 must be byte-identical to native once
// assembled and run through the real toolchain
// (tools/check_qbe_s3_straight_line.sh) — that needs `qbe`, only available
// in the dev-qbe container, so it is not a ctest here. This file covers
// what a plain C++ unit test can check without it: the emitted .ssa's
// structural shape (one exported function, a labeled block per leader, a
// slot address per height), and that every opcode this node does not lower
// throws, naming itself, rather than silently emitting nothing.

#include "backend/abstract_stack.h"
#include "backend/chunk_decoder.h"
#include "backend/qbe_emitter.h"
#include "compiler.h"
#include "memory_manager.h"
#include "object.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string emitScriptFrom(const std::string& source) {
    MemoryManager mm;
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        throw std::runtime_error("compilation failed");
    }
    DecodedFunction tree = decodeFunctionTree(script);
    FunctionStackAnalysis analysis = analyzeStack(tree);
    return qbe::emitScript(tree, analysis, "lox_fn_0");
}

// Emits one function nested inside the compiled program, found by walking
// DecodedFunction::nested by index at each step of `path` — e.g. {0} is the
// first function declared at top level, {0, 0} the first function declared
// inside that one. CLOSURE lives in the ENCLOSING function's own chunk;
// GET_UPVALUE/SET_UPVALUE/CLOSE_UPVALUE live in the CAPTURING function's
// own chunk — a test naming either needs the right node, not just the root.
std::string emitNestedFrom(const std::string& source,
                           const std::vector<int>& path) {
    MemoryManager mm;
    ObjFunction* script = compile(source, &mm);
    if (script == nullptr) {
        throw std::runtime_error("compilation failed");
    }
    DecodedFunction tree = decodeFunctionTree(script);
    const DecodedFunction* node = &tree;
    for (int idx : path) {
        node = &node->nested.at(static_cast<std::size_t>(idx));
    }
    FunctionStackAnalysis analysis = analyzeStack(*node);
    return qbe::emitScript(*node, analysis, "lox_fn_nested");
}

int countOccurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        pos += needle.size();
    }
    return count;
}

} // namespace

TEST(QbeEmitter, EmitsOneExportedFunctionMatchingRtCompiledFnShape) {
    std::string ssa = emitScriptFrom("print 1;");
    EXPECT_NE(ssa.find("export function w $lox_fn_0(l %rt, l %base) {"),
              std::string::npos);
    EXPECT_EQ(countOccurrences(ssa, "export function"), 1);
}

TEST(QbeEmitter, ConstantEmbedsRawDoubleBitsAtItsOwnHeight) {
    // CONSTANT 1.0 at height 1 (slot 0 is the script closure itself) ->
    // base+8, bit pattern 0x3FF0000000000000 = 4607182418800017408.
    std::string ssa = emitScriptFrom("print 1;");
    EXPECT_NE(ssa.find("storel 4607182418800017408"), std::string::npos);
}

TEST(QbeEmitter, EveryBlockEndsInATerminator) {
    // A block with no jmp/jnz/ret is invalid QBE input — every `@label`
    // line must be followed, before the next `@label` or the closing `}`,
    // by one of the three terminator forms.
    std::string ssa =
        emitScriptFrom("var i = 0; while (i < 3) { print i; i = i + 1; }");
    std::istringstream lines(ssa);
    std::string line;
    bool sawTerminatorSinceLabel = true;
    int labelCount = 0;
    while (std::getline(lines, line)) {
        if (!line.empty() && line[0] == '@') {
            EXPECT_TRUE(sawTerminatorSinceLabel)
                << "block before '" << line << "' has no terminator";
            sawTerminatorSinceLabel = false;
            labelCount++;
            continue;
        }
        if (line.find("jmp ") != std::string::npos ||
            line.find("jnz ") != std::string::npos ||
            line.find("ret ") != std::string::npos) {
            sawTerminatorSinceLabel = true;
        }
    }
    EXPECT_TRUE(sawTerminatorSinceLabel) << "final block has no terminator";
    EXPECT_GT(labelCount, 1) << "a while loop must produce more than one block";
}

TEST(QbeEmitter, JumpIfFalseBranchesOnBothTargets) {
    std::string ssa = emitScriptFrom("if (true) print 1; else print 2;");
    EXPECT_NE(ssa.find("jnz "), std::string::npos);
}

TEST(QbeEmitter, GlobalNameIsEmbeddedAsAnInternedDataString) {
    std::string ssa = emitScriptFrom("var g = 1; print g;");
    EXPECT_NE(ssa.find("data $lox_fn_0_name"), std::string::npos);
    EXPECT_NE(ssa.find("b \"g\""), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_op_define_global"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_op_get_global"), std::string::npos);
}

TEST(QbeEmitter, ArithmeticHasAFastAndSlowPath) {
    std::string ssa = emitScriptFrom("print 1 + 2;");
    EXPECT_NE(ssa.find("call $rt_op_add"), std::string::npos);
    EXPECT_NE(ssa.find(" =d add "), std::string::npos);
}

TEST(QbeEmitter, ModuloHasNoInlineFastPath) {
    // Q5: MODULO's floor-division rule is not a plain double-double op —
    // always the slow path, never an inline `rem`/`div`+`mul`+`sub` shape.
    std::string ssa = emitScriptFrom("print 5 % 2;");
    EXPECT_NE(ssa.find("call $rt_op_modulo"), std::string::npos);
    EXPECT_EQ(ssa.find(" =d rem "), std::string::npos);
}

TEST(QbeEmitter, ReturnSetsTopAndReturnsZero) {
    std::string ssa = emitScriptFrom("print 1;");
    EXPECT_NE(ssa.find("call $rt_set_top"), std::string::npos);
    EXPECT_NE(ssa.find("ret 0"), std::string::npos);
}

// Proves the checks below can fail: each names an opcode genuinely outside
// this node's scope (S5 the rest of the language, S6 errors), and the
// emitter must say so rather than emit silently wrong code for it.

TEST(QbeEmitter, EmitsBuildListRatherThanThrowing) {
    // BUILD_LIST is S5's job (notes/qbe-backend.md, "Staged plan", #458) —
    // out of scope for S4, but supported from here on: no throw, and the
    // slow-path call this opcode lowers to actually appears.
    std::string ssa = emitScriptFrom("var x = [1];");
    EXPECT_NE(ssa.find("rt_op_build_list"), std::string::npos);
}

TEST(QbeEmitter, ThrowsOnPushHandler) {
    // THROW/PUSH_HANDLER stay out of scope until S6 (#459) — the general
    // "names the unsupported opcode" behavior EmitsBuildListRatherThan
    // Throwing above no longer covers, now that S5 has closed the BUILD_LIST
    // gap that test used to exercise it with.
    EXPECT_THROW(emitScriptFrom("try { throw 1; } catch (e) { print e; }"),
                 std::runtime_error);
}

TEST(QbeEmitter, EmitsNonNumberConstantRatherThanThrowing) {
    // A String constant is read back via rt_constant_at rather than
    // re-encoded (S5, #458; bytecode-translation-problems.md P6,
    // "materialise") — no throw, and the read-back call actually appears.
    std::string ssa = emitScriptFrom("print \"hi\";");
    EXPECT_NE(ssa.find("rt_constant_at"), std::string::npos);
}

// ---------------------------------------------------------------------
// S4 (#457): CALL, RETURN's own callers, CLOSURE, upvalues, CLOSE_UPVALUE,
// and the stack-depth prologue check (Q3).
// ---------------------------------------------------------------------

TEST(QbeEmitter, EveryFunctionHasAStackDepthPrologueCheck) {
    // Q3, notes/qbe-backend.md: compiled code writes its own frame's slots
    // directly, bypassing push()'s own STACK_MAX check — every compiled
    // function's own prologue must ask rt_check_stack instead.
    std::string ssa = emitScriptFrom("print 1;");
    EXPECT_NE(ssa.find("call $rt_check_stack(l %rt"), std::string::npos);
}

TEST(QbeEmitter, CallSetsTopAndPassesArgCountAsAWord) {
    std::string ssa =
        emitScriptFrom("fun add(a, b) { return a + b; } print add(1, 2);");
    EXPECT_NE(ssa.find("call $rt_set_top"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_call(l %rt, w 2)"), std::string::npos);
}

TEST(QbeEmitter, ClosureReadsItsFunctionConstantThroughItsOwnClosure) {
    // CLOSURE has no constant pool of its own to read from at compile
    // time (rt_capi.h) — it must ask the currently executing closure, via
    // rt_current_closure, for its function constant, at runtime, by index.
    std::string ssa = emitScriptFrom("fun f() { return 1; } print f;");
    EXPECT_NE(ssa.find("call $rt_current_closure(l %rt)"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_constant_at(l %rt, l"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_new_closure(l %rt, l"), std::string::npos);
}

TEST(QbeEmitter, ClosureCapturesALocalUpvalue) {
    std::string ssa = emitNestedFrom(
        "fun outer() { var x = 1; fun inner() { return x; } return inner; }",
        {0});
    EXPECT_NE(ssa.find("call $rt_capture_local_upvalue(l %rt, l"),
              std::string::npos);
}

TEST(QbeEmitter, ClosureForwardsAnUpvalueFromItsOwnEnclosingFunction) {
    // `k` is captured by `mid` from `outer`, then forwarded (not
    // re-captured) into `inner` — the isLocal=0 case of CLOSURE's own
    // upvalue loop.
    std::string ssa = emitNestedFrom(
        "fun outer() { var k = 1; fun mid() { fun inner() { return k; } "
        "return inner; } return mid; }",
        {0, 0});
    EXPECT_NE(ssa.find("call $rt_forward_upvalue(l %rt, l"), std::string::npos);
}

TEST(QbeEmitter, GetUpvalueReadsThroughItsOwnClosure) {
    std::string ssa = emitNestedFrom(
        "fun outer() { var x = 1; fun inner() { return x; } return inner; }",
        {0, 0});
    EXPECT_NE(ssa.find("call $rt_get_upvalue(l %rt, l"), std::string::npos);
}

TEST(QbeEmitter, SetUpvalueLeavesValueOnStack) {
    // P2 (bytecode-translation-problems.md): SET_UPVALUE is an assignment
    // expression, so its own value must remain on the stack, not just be
    // written and discarded.
    std::string ssa = emitNestedFrom(
        "fun outer() { var x = 1; fun inner() { x = 2; return x; } return "
        "inner; }",
        {0, 0});
    EXPECT_NE(ssa.find("call $rt_set_upvalue(l %rt, l"), std::string::npos);
}

TEST(QbeEmitter, CloseUpvalueEmitsRtCloseUpvalues) {
    // A local captured inside a loop body gets a fresh cell per iteration
    // (P4, bytecode-translation-problems.md) — CLOSE_UPVALUE marks the end
    // of each iteration's own cell.
    std::string ssa = emitScriptFrom(
        "for (var i = 0; i < 3; i = i + 1) { var snapshot = i; fun f() { "
        "return snapshot; } }");
    EXPECT_NE(ssa.find("call $rt_close_upvalues(l %rt, l"), std::string::npos);
}

// ---------------------------------------------------------------------
// S5 (#458): classes, methods, aggregates, iterators, slicing, membership,
// match dispatch.
// ---------------------------------------------------------------------

TEST(QbeEmitter, MethodReadsItsConstantThroughRtCurrentClosureNotSlotZero) {
    // Regression: a method's own base[0] holds the receiver ("this"), not
    // its closure — unlike a plain CALL-entered function. Reading addr(0)
    // for "my own closure" (the pre-#458 CLOSURE/GET_UPVALUE/SET_UPVALUE
    // shortcut this node's CONSTANT/SET_PROPERTY lowering copied) hands
    // rt_constant_at an ObjInstance* it silently misreads as an
    // ObjClosure*, corrupting the property name pointer SET_PROPERTY then
    // dereferences. init()'s own SSA (id "0.0") must go through
    // rt_current_closure instead.
    std::string ssa =
        emitNestedFrom("class C { init(x) { this.x = x; } }", {0});
    EXPECT_NE(ssa.find("call $rt_current_closure(l %rt)"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_op_set_property(l %rt, l"), std::string::npos);
}

TEST(QbeEmitter, ClassAndInvokeLowerToTheirRtOpWrappers) {
    std::string ssa =
        emitScriptFrom("class C { get() { return 1; } } print C().get();");
    EXPECT_NE(ssa.find("call $rt_op_class(l %rt, l"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_op_invoke(l %rt, l"), std::string::npos);
}

TEST(QbeEmitter, SuperInvokeLowersToRtOpSuperInvoke) {
    // rt_op_inherit is emitted where the top-level script defines B < A;
    // super.greet() itself lives in B's own greet() method — both classes'
    // methods nest directly under the script (id "0.0" A.greet, "0.1"
    // B.greet), not under their own class.
    std::string script =
        "class A { greet() { return 1; } } "
        "class B < A { greet() { return super.greet() + 1; } } "
        "print B().greet();";
    EXPECT_NE(emitScriptFrom(script).find("call $rt_op_inherit(l %rt)"),
              std::string::npos);
    std::string methodSsa = emitNestedFrom(script, {1});
    EXPECT_NE(methodSsa.find("call $rt_op_super_invoke(l %rt, l"),
              std::string::npos);
}

TEST(QbeEmitter, SliceAndInLowerToTheirRtOpWrappers) {
    std::string ssa =
        emitScriptFrom("var s = \"hi\"; print s[0:1]; print 1 in [1];");
    EXPECT_NE(ssa.find("call $rt_op_slice(l %rt)"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_op_in(l %rt,"), std::string::npos);
}

TEST(QbeEmitter, JumpTableLowersToACompareChain) {
    // P8 (bytecode-translation-problems.md, hazard Q8): QBE has no switch
    // and no indirect jump — each arm becomes its own `ceqw`/`jnz` pair
    // rather than one dispatch instruction.
    std::string ssa = emitScriptFrom(
        "enum E { A B } var e = A(); var n = match e { case A => 0 case B "
        "=> 1 }; print n;");
    EXPECT_NE(ssa.find("call $rt_op_get_tag(l %rt)"), std::string::npos);
    EXPECT_NE(ssa.find("ceqw"), std::string::npos);
    // No QBE switch/jump-table construct exists to accidentally emit.
    EXPECT_EQ(ssa.find("switch"), std::string::npos);
}
