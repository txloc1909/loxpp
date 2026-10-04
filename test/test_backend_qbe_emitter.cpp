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
#include "backend/capture_analysis.h"
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
    FunctionCaptureInfo captures = analyzeCaptures(tree).functions.at(tree.id);
    return qbe::emitScript(tree, analysis, captures, "lox_fn_0");
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
    CaptureAnalysis allCaptures = analyzeCaptures(tree);
    return qbe::emitScript(*node, analysis, allCaptures.functions.at(node->id),
                           "lox_fn_nested");
}

std::string emitNestedWithOptions(const std::string& source,
                                  const std::vector<int>& path,
                                  const qbe::EmitOptions& options) {
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
    CaptureAnalysis allCaptures = analyzeCaptures(tree);
    return qbe::emitScript(*node, analysis, allCaptures.functions.at(node->id),
                           "lox_fn_nested", options);
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

// Returns the first instruction line of the label DEFINITION whose name
// contains `needle` (a definition starts with '@' at the start of a line),
// or an empty string when there is no such definition. A plain
// find(needle) is not enough: the same needle also appears as a jump-target
// reference on the `jnz` line just before the definition, so that
// reference must be skipped.
std::string labelBlockBody(const std::string& ssa, const std::string& needle) {
    std::size_t searchFrom = 0;
    for (;;) {
        std::size_t hit = ssa.find(needle, searchFrom);
        if (hit == std::string::npos) {
            return {};
        }
        std::size_t lineStart = ssa.rfind('\n', hit);
        lineStart = (lineStart == std::string::npos) ? 0 : lineStart + 1;
        if (ssa[lineStart] == '@') {
            std::size_t lineEnd = ssa.find('\n', lineStart);
            if (lineEnd == std::string::npos) {
                return {};
            }
            std::size_t nextLineEnd = ssa.find('\n', lineEnd + 1);
            std::size_t bodyEnd =
                (nextLineEnd == std::string::npos) ? ssa.size() : nextLineEnd;
            return ssa.substr(lineEnd + 1, bodyEnd - (lineEnd + 1));
        }
        searchFrom = hit + 1;
    }
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

TEST(QbeEmitter, TerminatorBlocksEndInATerminator) {
    // QBE allows a block to fall through to the next one (it inserts the
    // edge), so not every `@label` needs its own terminator — the S8
    // promotion work relies on that for the block before a `_tail` block.
    // But a `_tail` block holds the cfg block's real branch (emitTerminator),
    // and the function's own final block must end in a terminator.
    std::string ssa =
        emitScriptFrom("var i = 0; while (i < 3) { print i; i = i + 1; }");
    std::istringstream lines(ssa);
    std::string line;
    std::string currentLabel;
    std::string bodyLastLine;
    int labelCount = 0;
    auto isTerminator = [](const std::string& l) {
        return l.find("jmp ") != std::string::npos ||
               l.find("jnz ") != std::string::npos ||
               l.find("ret ") != std::string::npos;
    };
    auto flush = [&]() {
        if (currentLabel.size() > 5 &&
            currentLabel.compare(currentLabel.size() - 5, 5, "_tail") == 0) {
            EXPECT_TRUE(isTerminator(bodyLastLine))
                << "_tail block " << currentLabel
                << " has no terminator; last line: " << bodyLastLine;
        }
    };
    std::string lastNonEmpty;
    while (std::getline(lines, line)) {
        if (line.empty() || line == "}") {
            continue;
        }
        if (line[0] == '@') {
            flush();
            currentLabel = line;
            bodyLastLine.clear();
            labelCount++;
            continue;
        }
        bodyLastLine = line;
        lastNonEmpty = line;
    }
    flush();
    EXPECT_TRUE(isTerminator(lastNonEmpty))
        << "final block has no terminator: " << lastNonEmpty;
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

TEST(QbeEmitter, EmitsPushHandlerAndPopHandlerRatherThanThrowing) {
    // S6 (#459): try/catch is now emitted, not rejected. rt_push_handler
    // takes the checkpoint address only (no status check — it cannot
    // meaningfully fail). The try body completes normally (no throw), so
    // its normal-completion path — the one that actually reaches
    // POP_HANDLER — is reachable; a try body that only ever throws would
    // never fall through to it at all.
    std::string ssa = emitScriptFrom("try { print 1; } catch (e) { print e; }");
    EXPECT_NE(ssa.find("call $rt_push_handler(l %rt, l"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_pop_handler(l %rt)"), std::string::npos);
}

TEST(QbeEmitter, ThrowLowersToRtThrowRatherThanThrowing) {
    std::string ssa = emitScriptFrom("throw 1;");
    EXPECT_NE(ssa.find("call $rt_throw(l %rt, l"), std::string::npos);
}

TEST(QbeEmitter, LocalCatchJumpsToStaticallyActiveCatchBlockOnResumed) {
    // A fallible op inside a try's protected region must, on
    // Runtime::OpResult::Resumed (wire value 1 — the same throw resolved at
    // exactly this function's own frame), jump directly to the statically
    // active catch block (handler_depth.h's activeHandler) rather than
    // returning kRtThrow to propagate — proves the Catchability::Local path
    // actually reaches a `jmp @<label>`, not just any `jmp`/`ret`
    // anywhere in the function (a real bug this weaker check once missed:
    // the local-catch jump target was emitted without its own leading '@',
    // which every OTHER label reference in this file already has — `qbe`
    // itself rejected it as "unknown keyword", caught only by
    // tools/check_qbe_s6_errors.sh's real toolchain round-trip, not this
    // unit test, until this assertion was tightened to require the '@'
    // directly after the "_local" block's own label).
    std::string ssa =
        emitScriptFrom("try { print 1 + \"a\"; } catch (e) { print e; }");
    EXPECT_NE(ssa.find("ceqw"), std::string::npos)
        << "must compare the raw status against OpResult::Resumed(1)";
    // labelBlockBody finds the LABEL DEFINITION line, not the preceding
    // "jnz ..., @..._local24, @..._propagate24" jump-target reference that
    // also contains the substring "_local".
    std::string localBlockLine = labelBlockBody(ssa, "_local");
    EXPECT_NE(localBlockLine.find("jmp @L_"), std::string::npos)
        << "the local-catch block's own body must jump to a real cfg block "
           "label, with its own leading '@' — got: "
        << localBlockLine;
}

TEST(QbeEmitter, DeferRecordAndRunDefersAreEmitted) {
    // `g` (the second top-level function) is DecodedFunction::nested[1] —
    // DEFER_RECORD/RUN_DEFERS live in ITS OWN chunk, not the top-level
    // script's.
    std::string ssa = emitNestedFrom("fun h() {} fun g() { defer h(); }", {1});
    EXPECT_NE(ssa.find("call $rt_op_defer_record(l %rt, w"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_run_defers(l %rt, w"), std::string::npos);
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
    // S6 (#459): rt_call also takes this function's own stopAtFrameCount
    // temp (a runtime value, not a fixed literal — see qbe_emitter.h's own
    // comment on the status protocol), so the exact call site is
    // "call $rt_call(l %rt, w 2, w %<some temp>)", not a fixed string.
    EXPECT_NE(ssa.find("call $rt_call(l %rt, w 2, w %"), std::string::npos);
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
    EXPECT_NE(ssa.find("call $rt_op_slice(l %rt, w %"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_op_in(l %rt,"), std::string::npos);
}

TEST(QbeEmitter, SliceIsLocalCatchableNotFatal) {
    // opSlice dispatches __slice__, so SLICE must be lowered as
    // Catchability::Local with m_stopTemp, exactly like GET_INDEX/SET_INDEX.
    // A throw from the method, or its own arity error, must route to a live
    // catch block at this frame instead of returning kRtFatal. A revert to
    // Catchability::Fatal would still pass the wrapper-only check above.
    std::string ssa =
        emitScriptFrom("try { var s = \"hi\"; print s[0:1]; } catch (e) {}");
    EXPECT_NE(ssa.find("call $rt_op_slice(l %rt, w %"), std::string::npos)
        << "the stop depth must be this frame's own m_stopTemp register";
    std::string localBlockLine = labelBlockBody(ssa, "_local");
    EXPECT_NE(localBlockLine.find("jmp @L_"), std::string::npos)
        << "a slice inside a try must emit a local-catch block that jumps to "
           "the catch label, proving Catchability::Local — got: "
        << localBlockLine;
}

TEST(QbeEmitter, StrAndPrintLowerToRtOpStr) {
    // STR and PRINT both route through Runtime::opStr (which dispatches
    // __str__), so both lower to the opStr wrapper with this frame's own
    // stop depth.
    std::string ssa = emitScriptFrom("var s = str(1); print s;");
    EXPECT_NE(ssa.find("call $rt_op_str(l %rt, w %"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_op_print(l %rt, w %"), std::string::npos);
}

TEST(QbeEmitter, PrintIsLocalCatchableNotFatal) {
    // print routes through opStr, which can dispatch __str__ and throw, so
    // PRINT must be Catchability::Local with m_stopTemp — a throw inside a
    // printed value must route to a live catch block, not kRtFatal.
    std::string ssa = emitScriptFrom("try { print 1; } catch (e) {}");
    EXPECT_NE(ssa.find("call $rt_op_print(l %rt, w %"), std::string::npos)
        << "the stop depth must be this frame's own m_stopTemp register";
    std::string localBlockLine = labelBlockBody(ssa, "_local");
    EXPECT_NE(localBlockLine.find("jmp @L_"), std::string::npos)
        << "a print inside a try must emit a local-catch block that jumps to "
           "the catch label, proving Catchability::Local — got: "
        << localBlockLine;
}

TEST(QbeEmitter, JumpTableLowersToACompareChain) {
    // P8 (bytecode-translation-problems.md, hazard Q8): QBE has no switch
    // and no indirect jump — each arm becomes its own `ceqw`/`jnz` pair
    // rather than one dispatch instruction.
    std::string ssa = emitScriptFrom(
        "enum E { A B } var e = A(); var n = match e { case A => 0 case B "
        "=> 1 }; print n;");
    EXPECT_NE(ssa.find("call $rt_get_tag_word(l %rt, l"), std::string::npos);
    EXPECT_NE(ssa.find("ceqw"), std::string::npos);
    // No QBE switch/jump-table construct exists to accidentally emit.
    EXPECT_EQ(ssa.find("switch"), std::string::npos);
}

TEST(QbeEmitter, DenseMatchFusesGetTagIntoJumpTable) {
    // S8 (#461), P8: a dense enum match's GET_TAG is immediately followed
    // by JUMP_TABLE, so the pair lowers as one dispatch that reads the tag
    // as a word (rt_get_tag_word) — no boxed Number, so no `d cast` +
    // `dtosi` round trip back to a word at the branch.
    std::string ssa = emitScriptFrom(
        "enum E { A B C D } var e = C(); var n = match e { case A => 0 case "
        "B => 1 case C => 2 case D => 3 }; print n;");
    EXPECT_NE(ssa.find("call $rt_get_tag_word(l %rt, l"), std::string::npos);
    EXPECT_NE(ssa.find("csltw"), std::string::npos);
    EXPECT_EQ(ssa.find("call $rt_op_get_tag"), std::string::npos);
    EXPECT_EQ(ssa.find("dtosi"), std::string::npos);
}

TEST(QbeEmitter, SparseMatchKeepsTheUnfusedGetTag) {
    // A non-dense match (tags 0 and 3 with a catch-all) does not use
    // JUMP_TABLE, so there is nothing to fuse: the sequential GET_TAG /
    // CONSTANT / EQUAL chain still reads the boxed Number via
    // rt_op_get_tag. Proves the fusion check cannot fire on a lone
    // GET_TAG.
    std::string ssa = emitScriptFrom(
        "enum E { A B C D } var e = D(); var n = match e { case A => 1 case "
        "D => 4 case _ => 0 }; print n;");
    EXPECT_EQ(ssa.find("rt_get_tag_word"), std::string::npos);
    EXPECT_NE(ssa.find("call $rt_op_get_tag"), std::string::npos);
}

// ---------------------------------------------------------------------
// S8 (#461): register promotion + safe points.
// ---------------------------------------------------------------------

TEST(QbeEmitter, LoopCounterIsPromotedWithAPhi) {
    // A non-captured loop counter stays in a register across the back-edge:
    // its value at the loop header comes from a QBE phi rather than a stack
    // load, and the declaration/SET_LOCAL define plan-chosen `%q` values.
    std::string ssa = emitNestedFrom(
        "fun sum(n) { var i = 0; while (i < n) { i = i + 1; } return i; }",
        {0});
    EXPECT_NE(ssa.find("=l phi"), std::string::npos);
    EXPECT_NE(ssa.find("%qp1"), std::string::npos);
}

TEST(QbeEmitter, CapturedParamIsNotPromoted) {
    // A slot a nested closure captures must keep its stack slot — the VM's
    // own captureUpvalue points into it, so promotion would break the
    // upvalue and the GC. `p` is captured, so no `%qp1` (the parameter's
    // register value) is ever defined, even though other locals still are.
    std::string ssa = emitNestedFrom(
        "fun outer(p) { fun inner() { return p; } return inner; }", {0});
    EXPECT_NE(ssa.find("call $rt_capture_local_upvalue(l %rt, l"),
              std::string::npos);
    EXPECT_EQ(ssa.find("%qp1"), std::string::npos)
        << "a captured parameter must keep its stack slot";
}

TEST(QbeEmitter, PromotedLocalIsSpilledBeforeACall) {
    // Q1: an allocating call is a safe point. A promoted local live across
    // it must be written back to its stack slot first, or the GC could miss
    // it. `f` calls `g` while its own promoted local `x` is live, so a
    // `storel %q` (the spill) must come BEFORE the call — a plain GET_LOCAL
    // of `x` at the return would also store `%q`, but after it.
    std::string ssa = emitNestedFrom(
        "fun g() { return 1; } fun f() { var x = 1; g(); return x; }", {1});
    std::size_t spill = ssa.find("storel %q");
    std::size_t call = ssa.find("call $rt_call");
    ASSERT_NE(spill, std::string::npos) << "no promoted-local store at all";
    ASSERT_NE(call, std::string::npos) << "no call to spill before";
    EXPECT_LT(spill, call)
        << "the promoted local must be spilled before the allocating call";
}

TEST(QbeEmitter, CoroutineModeEmitsResumeDispatchAndYield) {
    // A YIELD-containing function compiled in coroutine mode: the prologue
    // dispatches on the frame's resume point, the YIELD records its own
    // resume point, and a legal yield returns kRtYield (4) to the driver.
    qbe::EmitOptions options;
    options.coroutineMode = true;
    std::string ssa =
        emitNestedWithOptions("fun gen() { yield 1; return 2; }", {0}, options);
    EXPECT_NE(ssa.find("call $rt_resume_state(l %rt)"), std::string::npos)
        << "the prologue must read the frame's resume point";
    EXPECT_NE(ssa.find("call $rt_set_resume_state(l %rt, w"), std::string::npos)
        << "the YIELD must record its own resume point";
    EXPECT_NE(ssa.find("call $rt_yield(l %rt"), std::string::npos);
    EXPECT_NE(ssa.find("\tret 4\n"), std::string::npos) // kRtYield
        << "a legal yield returns kRtYield to the driver";
    EXPECT_NE(ssa.find("lox_fn_nested_resume"), std::string::npos)
        << "the resume continuation needs its own label";
    EXPECT_EQ(ssa.find("%qp"), std::string::npos)
        << "promotion must be off in coroutine mode";
}

TEST(QbeEmitter, CoroutineModeEmitsCallStatusHandling) {
    // A resumable call must record its resume point and return kRtCall (3)
    // when the runtime pushes the callee for the driver instead of running it.
    qbe::EmitOptions options;
    options.coroutineMode = true;
    std::string ssa = emitNestedWithOptions(
        "fun g() { return 1; } fun f() { return g(); }", {1}, options);
    EXPECT_NE(ssa.find("call $rt_call(l %rt"), std::string::npos);
    EXPECT_NE(ssa.find("\tret 3\n"), std::string::npos) // kRtCall
        << "a resumable call must return kRtCall to the driver";
}

TEST(QbeEmitter, YieldOutsideCoroutineModeThrows) {
    qbe::EmitOptions options; // coroutineMode defaults false
    EXPECT_THROW(emitNestedWithOptions("fun gen() { yield 1; }", {0}, options),
                 std::runtime_error);
}
