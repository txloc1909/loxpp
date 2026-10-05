// test_backend_rt_capi.cpp — the QBE backend's C interface (S2):
// rt_attach_code's id/arity/chunk-hash verification, rt_startup's
// embed-and-recompile path, and rt_call's CallFrame push/pop around a
// compiled closure. No real QBE-generated code exists yet, so the
// "compiled" functions under test here are hand-written C++ that match
// RtCompiledFn's signature — exactly what a QBE .ssa function looks like
// from the runtime's side of the C ABI.

#include "backend/rt_capi.h"
#include "backend/rt_abi.h"
#include "backend/chunk_decoder.h"
#include "runtime.h"
#include "compiler.h"
#include "memory_manager.h"
#include "exec_objects.h"
#include "class_objects.h"
#include "container_objects.h"

#include <gtest/gtest.h>

namespace {

// Records how a fake compiled function was invoked, so a test can assert
// the CallFrame it saw without a real QBE-compiled body to inspect.
struct FakeCallRecord {
    int calls = 0;
    Runtime* rt = nullptr;
    Value* base = nullptr;
};

FakeCallRecord* g_record = nullptr;

int fakeCompiledOk(Runtime* rt, Value* base) {
    g_record->calls++;
    g_record->rt = rt;
    g_record->base = base;
    // The C-ABI return convention (rt_abi.h): leave the return value as the
    // single value on top of the stack before returning 0.
    rt->push(Value{});
    return 0;
}

int fakeCompiledFatal(Runtime* rt, Value* base) {
    g_record->calls++;
    g_record->rt = rt;
    g_record->base = base;
    // A real compiled function that fails uncaught reaches this contract
    // the same way an interpreted RETURN's own error paths do: it reports
    // through runtimeError() (which resets the whole Runtime) before
    // returning kRtFatal (rt_abi.h's three-way status — S6, #459).
    // callCompiled relies on that — it does not reset anything itself on a
    // kRtFatal return (see callCompiled's own comment).
    rt->runtimeError("fake fatal error");
    return kRtFatal;
}

int fakeCompiledThrows(Runtime*, Value*) {
    throw 42; // a stray C++ exception, not a reported runtime error (R3)
}

int fakeCompiledThrowStatus(Runtime*, Value*) {
    // Simulates a compiled callee whose own frame already unwound (S6,
    // #459): a real compiled function returns kRtThrow only when the
    // handler resolving its own fault is not its own frame — see
    // qbe_emitter.cpp's local-catch-or-propagate codegen and rt_abi.h's own
    // comment on RtCompiledFn. This fake body does none of that unwinding
    // itself (there is no real handler stack in this test); it only proves
    // callCompiled's own translation of kRtThrow back into OpResult, given
    // whatever m_frameCount/stopAtFrameCount it is called with.
    return kRtThrow;
}

ObjUpvalue* g_capturedUpvalue = nullptr;

int fakeCompiledCapturesOwnLocal(Runtime* rt, Value* base) {
    // Simulate a nested closure capturing this compiled function's own
    // local (the pattern CLOSURE's isLocal upvalues build — S4's job).
    rt->push(Value{42.0}); // base[1]: the captured local
    g_capturedUpvalue = rt->captureUpvalue(&base[1]);
    rt->push(Value{}); // return value: nil
    return 0;
}

int fakeCompiledReturnsComputedValue(Runtime* rt, Value*) {
    // Two locals a real compiled body might declare, to prove they do not
    // leak past the callee's own return (R2) — only the last value pushed,
    // the return value, must survive callCompiled's frame collapse.
    rt->push(Value{1.0});
    rt->push(Value{2.0});
    rt->push(Value{99.0});
    return 0;
}

// Finds "0.0" — the first function constant in a one-function script's own
// chunk — inside a decoded tree.
const DecodedFunction* findFirstNested(const DecodedFunction& root) {
    return root.nested.empty() ? nullptr : &root.nested.front();
}

} // namespace

// ===========================================================================
// rt_call / Runtime::callCompiled — CallFrame push and pop
// ===========================================================================

TEST(RtCallCompiled, PushesFrameCallsCodeAndPopsOnSuccess) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->arity = 2;
    fn->code = reinterpret_cast<void*>(&fakeCompiledOk);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.memoryManager().popTempRoot();

    FakeCallRecord record;
    g_record = &record;

    Value* calleeSlot = rt.top();
    rt.push(Value{static_cast<Obj*>(closure)});
    rt.push(Value{1.0});
    rt.push(Value{2.0});

    int framesBefore = rt.frameCount();
    int status = rt_call(&rt, 2);

    EXPECT_EQ(status, static_cast<int>(Runtime::OpResult::OK));
    EXPECT_EQ(rt.frameCount(), framesBefore) << "frame must be popped again";
    EXPECT_EQ(record.calls, 1);
    EXPECT_EQ(record.rt, &rt);
    // slots = stackTop - argc - 1: slot 0 is the callee itself.
    EXPECT_EQ(record.base, calleeSlot);
}

TEST(RtCallCompiled, NonzeroReturnIsFatalAndLeavesRuntimeAlreadyReset) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->arity = 0;
    fn->code = reinterpret_cast<void*>(&fakeCompiledFatal);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.memoryManager().popTempRoot();

    FakeCallRecord record;
    g_record = &record;

    rt.push(Value{static_cast<Obj*>(closure)});
    int status = rt_call(&rt, 0);

    EXPECT_EQ(status, static_cast<int>(Runtime::OpResult::Fatal));
    EXPECT_EQ(record.calls, 1);
    // A nonzero return means the callee already reported the error through
    // runtimeError(), which already reset the whole Runtime (resetStack()).
    // callCompiled must not touch m_frameCount again on this path — doing
    // so would double-decrement a count that is already 0.
    EXPECT_EQ(rt.frameCount(), 0);
}

// S6 (#459): kRtThrow's own translation back into OpResult, exactly the
// way fromThrow() translates ThrowOutcome for every other call site —
// Resumed when this invocation's own context is still live (m_frameCount >
// stopAtFrameCount), Stop otherwise.
TEST(RtCallCompiled, ThrowStatusAboveStopBoundaryReportsResumed) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->arity = 0;
    fn->code = reinterpret_cast<void*>(&fakeCompiledThrowStatus);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.memoryManager().popTempRoot();

    rt.push(Value{static_cast<Obj*>(closure)});
    // stopAtFrameCount=0: after call() pushes this call's own frame,
    // m_frameCount is 1, strictly above the boundary.
    int status = rt_call(&rt, 0, /*stopAtFrameCount=*/0);

    EXPECT_EQ(status, static_cast<int>(Runtime::OpResult::Resumed));
}

TEST(RtCallCompiled, ThrowStatusAtOrBelowStopBoundaryReportsStop) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->arity = 0;
    fn->code = reinterpret_cast<void*>(&fakeCompiledThrowStatus);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.memoryManager().popTempRoot();

    rt.push(Value{static_cast<Obj*>(closure)});
    // stopAtFrameCount=1: m_frameCount after the push is also 1, at (not
    // above) the boundary.
    int status = rt_call(&rt, 0, /*stopAtFrameCount=*/1);

    EXPECT_EQ(status, static_cast<int>(Runtime::OpResult::Stop));
}

// R1 (blocking, PR #473 review round 1): callCompiled skipped
// closeUpvalues(frame->slots) before popping the frame, unlike Op::RETURN
// (vm.cpp). A nested closure capturing one of a compiled function's own
// locals stayed open across the return, so a later, unrelated push() into
// the reused stack slot was silently observed through the "captured" value.
TEST(RtCallCompiled, ClosesUpvaluesCapturedOverItsOwnFrameOnReturn) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->arity = 0;
    fn->code = reinterpret_cast<void*>(&fakeCompiledCapturesOwnLocal);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.memoryManager().popTempRoot();

    rt.push(Value{static_cast<Obj*>(closure)});
    g_capturedUpvalue = nullptr;

    int status = rt_call(&rt, 0);

    EXPECT_EQ(status, static_cast<int>(Runtime::OpResult::OK));
    ASSERT_NE(g_capturedUpvalue, nullptr);
    EXPECT_EQ(g_capturedUpvalue->location, &g_capturedUpvalue->closed)
        << "the upvalue must be closed before callCompiled pops the frame, "
           "or a later stack reuse silently corrupts the captured value";
    ASSERT_TRUE(is<Number>(g_capturedUpvalue->closed));
    EXPECT_EQ(as<Number>(g_capturedUpvalue->closed), 42.0);
}

// R2 (blocking, PR #473 review round 1): pins down the return-value
// convention Op::RETURN mirrors — before returning 0, a compiled function
// leaves its return value as the single value on top of the stack.
// callCompiled then collapses the callee's whole window (locals included)
// and leaves exactly that one value at the frame's own base slot.
TEST(RtCallCompiled, ReturnConventionLeavesExactlyOneValueAtFrameBase) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->arity = 0;
    fn->code = reinterpret_cast<void*>(&fakeCompiledReturnsComputedValue);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.memoryManager().popTempRoot();

    Value* base = rt.top();
    rt.push(Value{static_cast<Obj*>(closure)});

    int status = rt_call(&rt, 0);

    EXPECT_EQ(status, static_cast<int>(Runtime::OpResult::OK));
    EXPECT_EQ(rt.top(), base + 1)
        << "compiled locals must not leak past the callee's own return";
    Value result = rt_pop(&rt);
    ASSERT_TRUE(is<Number>(result));
    EXPECT_EQ(as<Number>(result), 99.0);
}

// R3 (blocking, PR #473 review round 1): rtGuard's catch turned a stray
// C++ exception into Fatal but left m_frameCount inconsistent — unlike
// every other path to Fatal, which goes through runtimeError()'s own
// resetStack(). A compiled function's body can throw partway through
// callCompiled, after call() already pushed the CallFrame.
TEST(RtCallCompiled, ExceptionDuringCallLeavesFrameCountConsistent) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->arity = 0;
    fn->code = reinterpret_cast<void*>(&fakeCompiledThrows);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.memoryManager().popTempRoot();

    rt.push(Value{static_cast<Obj*>(closure)});

    int status = rt_call(&rt, 0);

    EXPECT_EQ(status, static_cast<int>(Runtime::OpResult::Fatal));
    EXPECT_EQ(rt.frameCount(), 0)
        << "rtGuard must reset the Runtime (resetStack()), not just report "
           "Fatal, on a caught exception — otherwise the frame count leaks";
}

// Proves the check can fail: an arity mismatch must reject the call before
// ever reaching the compiled code, the same as it does for an interpreted
// closure.
TEST(RtCallCompiled, ArityMismatchNeverInvokesCode) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->arity = 2;
    fn->code = reinterpret_cast<void*>(&fakeCompiledOk);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.memoryManager().popTempRoot();

    FakeCallRecord record;
    g_record = &record;

    rt.push(Value{static_cast<Obj*>(closure)});
    rt.push(Value{1.0}); // one argument; fn expects two

    int status = rt_call(&rt, 1);

    EXPECT_EQ(status, static_cast<int>(Runtime::OpResult::Fatal));
    EXPECT_EQ(record.calls, 0);
}

// ===========================================================================
// rt_attach_code — id/arity/chunk-hash verification
// ===========================================================================

TEST(RtAttachCode, AttachesOnMatchingIdArityHash) {
    MemoryManager mm;
    ObjFunction* root = compile("fun add(a, b) { return a + b; }", &mm);
    ASSERT_NE(root, nullptr);
    DecodedFunction tree = decodeFunctionTree(root);
    const DecodedFunction* addNode = findFirstNested(tree);
    ASSERT_NE(addNode, nullptr);
    EXPECT_EQ(addNode->id, "0.0");

    RtFunctionDesc desc{};
    desc.id = "0.0";
    desc.arity = addNode->function->arity;
    desc.chunkHash = hashChunkBytes(addNode->function->chunk);
    desc.code = reinterpret_cast<void*>(&fakeCompiledOk);

    EXPECT_TRUE(rt_attach_code(tree, desc));
    EXPECT_EQ(addNode->function->code, desc.code);
}

TEST(RtAttachCode, RejectsArityMismatchAndDoesNotAttach) {
    MemoryManager mm;
    ObjFunction* root = compile("fun add(a, b) { return a + b; }", &mm);
    DecodedFunction tree = decodeFunctionTree(root);
    const DecodedFunction* addNode = findFirstNested(tree);
    ASSERT_NE(addNode, nullptr);

    RtFunctionDesc desc{};
    desc.id = "0.0";
    desc.arity = addNode->function->arity + 1; // wrong on purpose
    desc.chunkHash = hashChunkBytes(addNode->function->chunk);
    desc.code = reinterpret_cast<void*>(&fakeCompiledOk);

    EXPECT_FALSE(rt_attach_code(tree, desc));
    EXPECT_EQ(addNode->function->code, nullptr);
}

TEST(RtAttachCode, RejectsChunkHashMismatchAndDoesNotAttach) {
    MemoryManager mm;
    ObjFunction* root = compile("fun add(a, b) { return a + b; }", &mm);
    DecodedFunction tree = decodeFunctionTree(root);
    const DecodedFunction* addNode = findFirstNested(tree);
    ASSERT_NE(addNode, nullptr);

    RtFunctionDesc desc{};
    desc.id = "0.0";
    desc.arity = addNode->function->arity;
    desc.chunkHash = hashChunkBytes(addNode->function->chunk) ^ 0xFULL;
    desc.code = reinterpret_cast<void*>(&fakeCompiledOk);

    EXPECT_FALSE(rt_attach_code(tree, desc));
    EXPECT_EQ(addNode->function->code, nullptr);
}

TEST(RtAttachCode, RejectsUnknownId) {
    MemoryManager mm;
    ObjFunction* root = compile("var x = 1;", &mm);
    DecodedFunction tree = decodeFunctionTree(root);

    RtFunctionDesc desc{};
    desc.id = "0.99";
    desc.arity = 0;
    desc.chunkHash = 0;
    desc.code = reinterpret_cast<void*>(&fakeCompiledOk);

    EXPECT_FALSE(rt_attach_code(tree, desc));
}

// ===========================================================================
// rt_startup — embed-and-recompile, end to end
// ===========================================================================

TEST(RtStartup, CompilesAndReturnsARuntime) {
    Runtime* rt = rt_startup("var x = 1;", nullptr, 0);
    ASSERT_NE(rt, nullptr);
    rt_shutdown(rt);
}

TEST(RtStartup, ReturnsNullOnCompileError) {
    Runtime* rt = rt_startup("fun (", nullptr, 0);
    EXPECT_EQ(rt, nullptr);
}

TEST(RtStartup, AttachesMatchingDescsAndRejectsAMismatch) {
    const char* source = "fun add(a, b) { return a + b; }";
    MemoryManager mm;
    ObjFunction* root = compile(source, &mm);
    ASSERT_NE(root, nullptr);
    DecodedFunction tree = decodeFunctionTree(root);
    const DecodedFunction* addNode = findFirstNested(tree);
    ASSERT_NE(addNode, nullptr);

    RtFunctionDesc desc{};
    desc.id = "0.0";
    desc.arity = addNode->function->arity;
    desc.chunkHash = hashChunkBytes(addNode->function->chunk);
    desc.code = reinterpret_cast<void*>(&fakeCompiledOk);

    Runtime* ok = rt_startup(source, &desc, 1);
    EXPECT_NE(ok, nullptr);
    if (ok != nullptr) {
        rt_shutdown(ok);
    }

    desc.arity = 99;
    Runtime* rejected = rt_startup(source, &desc, 1);
    EXPECT_EQ(rejected, nullptr);
}

// ===========================================================================
// rt_op_print / rt_new_string / rt_call(native) — this node's own checkpoint
// ===========================================================================

// ===========================================================================
// rt_startup's requireAllCompiled parameter (S4, #457, hazard R4 on #457)
// ===========================================================================

TEST(RtStartup, DefaultsToNotRequiringEveryFunctionCompiled) {
    // Unchanged behavior for every existing caller (this file's own tests
    // above, S2/S3's checkpoint harnesses): a program with an uncompiled
    // nested function still starts up when requireAllCompiled is left at
    // its default.
    Runtime* rt = rt_startup("fun add(a, b) { return a + b; }", nullptr, 0);
    ASSERT_NE(rt, nullptr);
    rt_shutdown(rt);
}

TEST(RtStartup, RequireAllCompiledRejectsAnUncompiledNestedFunction) {
    Runtime* rt = rt_startup("fun add(a, b) { return a + b; }", nullptr, 0,
                             /*requireAllCompiled=*/true);
    EXPECT_EQ(rt, nullptr);
}

TEST(RtStartup, RequireAllCompiledAcceptsAProgramWhereEveryFunctionHasCode) {
    const char* source = "fun add(a, b) { return a + b; }";
    MemoryManager mm;
    ObjFunction* root = compile(source, &mm);
    ASSERT_NE(root, nullptr);
    DecodedFunction tree = decodeFunctionTree(root);
    const DecodedFunction* addNode = findFirstNested(tree);
    ASSERT_NE(addNode, nullptr);

    // Every function in the tree needs a desc: the top-level script (root)
    // as well as "add" — the same requirement a whole-program driver's own
    // multi-function harness must satisfy.
    RtFunctionDesc rootDesc{};
    rootDesc.id = tree.id.c_str();
    rootDesc.arity = root->arity;
    rootDesc.chunkHash = hashChunkBytes(root->chunk);
    rootDesc.code = reinterpret_cast<void*>(&fakeCompiledOk);

    RtFunctionDesc addDesc{};
    addDesc.id = "0.0";
    addDesc.arity = addNode->function->arity;
    addDesc.chunkHash = hashChunkBytes(addNode->function->chunk);
    addDesc.code = reinterpret_cast<void*>(&fakeCompiledOk);

    RtFunctionDesc descs[] = {rootDesc, addDesc};
    Runtime* rt = rt_startup(source, descs, 2, /*requireAllCompiled=*/true);
    ASSERT_NE(rt, nullptr);
    rt_shutdown(rt);
}

// ===========================================================================
// CLOSURE/upvalue wrappers (S4, #457) — no capture analysis: these reach
// the VM's own existing captureUpvalue/closeUpvalues mechanism unchanged.
// ===========================================================================

TEST(RtCapiClosureWrappers, NewClosureAllocatesWithEveryUpvalueSlotNull) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->upvalueCount = 2;

    Value closureVal = rt_new_closure(&rt, Value{static_cast<Obj*>(fn)});
    rt.memoryManager().popTempRoot();
    ASSERT_TRUE(isClosure(closureVal));
    ObjClosure* closure = asObjClosure(closureVal);
    EXPECT_EQ(closure->function, fn);
    ASSERT_EQ(closure->upvalues.size(), 2u);
    EXPECT_EQ(closure->upvalues[0], nullptr);
    EXPECT_EQ(closure->upvalues[1], nullptr);
}

TEST(RtCapiClosureWrappers, ConstantAtReadsFromTheClosuresOwnFunction) {
    MemoryManager mm;
    ObjFunction* root = compile("fun add(a, b) { return a + b; }", &mm);
    ASSERT_NE(root, nullptr);
    // Find the FUNCTION constant CLOSURE's own operand names — its index
    // among root's pooled constants is a compiler detail, not something
    // this test should hardcode.
    const auto& pool = root->chunk.constants();
    int functionConstantIndex = -1;
    for (uint16_t i = 0; i < pool.size(); i++) {
        if (isFunction(pool.at(i))) {
            functionConstantIndex = static_cast<int>(i);
            break;
        }
    }
    ASSERT_GE(functionConstantIndex, 0)
        << "compiler drift: no FUNCTION constant in root's own chunk";

    Runtime rt;
    // Reuse root's own chunk as the "currently executing" function for
    // this test — rt_constant_at only reads through the closure passed to
    // it, so any ObjFunction with the right constant pool works.
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(root);

    Value constant = rt_constant_at(&rt, Value{static_cast<Obj*>(closure)},
                                    functionConstantIndex);
    EXPECT_TRUE(isFunction(constant));
}

TEST(RtCapiClosureWrappers, CaptureLocalUpvalueStoresARealUpvalue) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->upvalueCount = 1;
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    // Transfer the root from fn to closure (no allocation in between, so
    // this is safe): closure itself is never pushed onto rt's own value
    // stack in this test, so it needs its own explicit root for the rest
    // of the test body, not just across its own construction — the same
    // is true of the three sibling tests below. Left un-popped: harmless,
    // this Runtime (and its MemoryManager) is destroyed at the end of the
    // test either way.
    rt.memoryManager().popTempRoot();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(closure));
    Value closureVal{static_cast<Obj*>(closure)};

    rt.push(Value{7.0});
    Value* localSlot = rt.top() - 1;
    rt_capture_local_upvalue(&rt, closureVal, 0, localSlot);

    ASSERT_NE(closure->upvalues[0], nullptr);
    EXPECT_EQ(closure->upvalues[0]->location, localSlot);
}

TEST(RtCapiClosureWrappers, ForwardUpvalueCopiesThePointerNotTheValue) {
    Runtime rt;
    ObjFunction* parentFn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(parentFn));
    parentFn->upvalueCount = 1;
    ObjClosure* parent = rt.memoryManager().create<ObjClosure>(parentFn);
    rt.memoryManager().popTempRoot();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(parent));
    Value parentVal{static_cast<Obj*>(parent)};

    rt.push(Value{3.0});
    ObjUpvalue* uv = rt.captureUpvalue(rt.top() - 1);
    parent->upvalues[0] = uv;

    ObjFunction* childFn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(childFn));
    childFn->upvalueCount = 1;
    ObjClosure* child = rt.memoryManager().create<ObjClosure>(childFn);
    rt.memoryManager().popTempRoot();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(child));
    Value childVal{static_cast<Obj*>(child)};

    rt_forward_upvalue(&rt, childVal, 0, parentVal, 0);

    EXPECT_EQ(child->upvalues[0], uv);
}

TEST(RtCapiClosureWrappers, GetAndSetUpvalueRoundTripThroughTheSameCell) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->upvalueCount = 1;
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.memoryManager().popTempRoot();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(closure));
    Value closureVal{static_cast<Obj*>(closure)};

    rt.push(Value{1.0});
    closure->upvalues[0] = rt.captureUpvalue(rt.top() - 1);

    rt_set_upvalue(&rt, closureVal, 0, Value{9.0});
    Value read = rt_get_upvalue(&rt, closureVal, 0);
    ASSERT_TRUE(is<Number>(read));
    EXPECT_EQ(as<Number>(read), 9.0);
    // The write went through the open upvalue's own location — the local
    // slot itself, not a private copy.
    ASSERT_TRUE(is<Number>(*(rt.top() - 1)));
    EXPECT_EQ(as<Number>(*(rt.top() - 1)), 9.0);
}

TEST(RtCapiClosureWrappers, CloseUpvaluesClosesEveryOpenCellAtOrAboveLast) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(fn));
    fn->upvalueCount = 1;
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.memoryManager().popTempRoot();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(closure));

    rt.push(Value{5.0});
    Value* slot = rt.top() - 1;
    ObjUpvalue* uv = rt.captureUpvalue(slot);
    closure->upvalues[0] = uv;

    rt_close_upvalues(&rt, slot);

    EXPECT_EQ(uv->location, &uv->closed);
    ASSERT_TRUE(is<Number>(uv->closed));
    EXPECT_EQ(as<Number>(uv->closed), 5.0);
}

// ===========================================================================
// rt_check_stack (S4, #457, Q3) — mirrors Runtime::call()'s own FRAMES_MAX
// guard, but for the value stack: compiled code writes its own frame's
// slots directly, bypassing push()'s own STACK_MAX check entirely.
// ===========================================================================

TEST(RtCheckStack, SucceedsWellWithinStackMax) {
    Runtime rt;
    Value* needed = rt.stackBase() + 8;
    EXPECT_EQ(rt_check_stack(&rt, needed, 0),
              static_cast<int>(Runtime::OpResult::OK));
}

TEST(RtCheckStack, FailsPastStackMaxWithNoHandlerActive) {
    Runtime rt;
    // Comfortably past STACK_MAX (loxpp::kStackMax) with no handler
    // active: the same uncaught-fatal path push() would eventually surface
    // through the dispatch loop's own m_stackOverflow flag, but reached
    // directly here instead, since compiled code never sets that flag.
    Value* needed = rt.stackBase() + loxpp::kStackMax + 1;
    EXPECT_EQ(rt_check_stack(&rt, needed, 0),
              static_cast<int>(Runtime::OpResult::Fatal));
}

TEST(RtCapiCheckpoint, PrintsAStringAndCallsAStdlibFunction) {
    Runtime* rt = rt_startup("", nullptr, 0);
    ASSERT_NE(rt, nullptr);

    Value greeting = rt_new_string(rt, "hello from qbe");
    rt_push(rt, greeting);
    testing::internal::CaptureStdout();
    int printStatus = rt_op_print(rt, 0);
    std::string printed = testing::internal::GetCapturedStdout();
    EXPECT_EQ(printStatus, static_cast<int>(Runtime::OpResult::OK));
    EXPECT_EQ(printed, "hello from qbe\n");

    Value clockFn = rt_get_global(rt, "clock");
    ASSERT_TRUE(isNative(clockFn));
    rt_push(rt, clockFn);
    int callStatus = rt_call(rt, 0);
    EXPECT_EQ(callStatus, static_cast<int>(Runtime::OpResult::OK));
    Value result = rt_pop(rt);
    EXPECT_TRUE(is<Number>(result));

    rt_shutdown(rt);
}

// ===========================================================================
// runPendingDefers() — BoundMethod/Closure branches go through
// invokeClosure(), not a bare call()
// ===========================================================================
//
// QBE has no Op::DEFER_RECORD/Op::RUN_DEFERS lowering yet (S6, #459), so
// these two branches are unreachable from a QBE-compiled callER today. But
// their own callEE can still be QBE-compiled: rt_capi.h's own
// "interpreted-fallback closure (function->code == nullptr)" split means an
// interpreted function (still dispatched through VM::run(), the only place
// that calls Op::RUN_DEFERS) can defer a call onto a sibling closure that
// DID compile successfully. A bare call() on that closure would only push a
// CallFrame and return — the same "pushed frame nobody runs" bug
// invokeClosure() fixes at every other direct-call site in runtime.cpp —
// so these tests attach real compiled code to the deferred callee and prove
// runPendingDefers() actually runs it, not silently no-ops.

// Grants this test file the same private access VM has (friend struct
// VMTestAccess; in runtime.h) to build a deferred call the way
// Op::DEFER_RECORD does (vm.cpp), without routing through a full VM.
struct VMTestAccess {
    static void recordDefer(Runtime& rt, int frameIndex, Value callee) {
        ObjDeferredCall* deferred = rt.m_mm.create<ObjDeferredCall>(
            callee, VmAllocator<Value>{&rt.m_mm});
        rt.m_deferLists[frameIndex].push_back(
            Value{static_cast<Obj*>(deferred)});
    }
};

namespace {

int fakeCompiledDeferTarget(Runtime* rt, Value*) {
    g_record->calls++;
    g_record->rt = rt;
    rt->push(Value{}); // C-ABI return convention: nil.
    return 0;
}

// Pushes an interpreted "outer" frame (function->code == nullptr) at
// frameIndex 0 — the frame whose RUN_DEFERS drains the list under test.
// Its own body never runs; only its CallFrame/defer-list slot needs to
// exist for runPendingDefers(0, ...) to be a valid call.
ObjClosure* pushInterpretedOuterFrame(Runtime& rt) {
    ObjFunction* outerFn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(outerFn));
    outerFn->arity = 0;
    ObjClosure* outer = rt.memoryManager().create<ObjClosure>(outerFn);
    rt.memoryManager().popTempRoot();
    rt.push(Value{static_cast<Obj*>(outer)});
    EXPECT_EQ(rt.call(outer, 0), Runtime::ThrowOutcome::Pushed);
    return outer;
}

} // namespace

TEST(RunPendingDefers, ClosureBranchRunsAttachedCompiledCodeNotANoop) {
    Runtime rt;
    pushInterpretedOuterFrame(rt);
    ASSERT_EQ(rt.frameCount(), 1);

    ObjFunction* deferredFn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(deferredFn));
    deferredFn->arity = 0;
    deferredFn->code = reinterpret_cast<void*>(&fakeCompiledDeferTarget);
    ObjClosure* deferredClosure =
        rt.memoryManager().create<ObjClosure>(deferredFn);
    // Keep deferredClosure itself rooted through recordDefer's own
    // allocation below (no allocation between this pop/push pair, so
    // transferring the root here is safe) — it is never on rt's own value
    // stack in this test.
    rt.memoryManager().popTempRoot();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(deferredClosure));

    FakeCallRecord record;
    g_record = &record;
    VMTestAccess::recordDefer(rt, /*frameIndex=*/0,
                              Value{static_cast<Obj*>(deferredClosure)});

    InterpretResult result = rt.runPendingDefers(0, 0);

    EXPECT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(record.calls, 1)
        << "a bare call() on a compiled closure only pushes a CallFrame and "
           "returns — nothing would ever run it, so the compiled body would "
           "silently never execute";
    EXPECT_EQ(rt.frameCount(), 1) << "the deferred call's own frame must be "
                                     "pushed and popped, not leaked";
}

TEST(RunPendingDefers, BoundMethodBranchRunsAttachedCompiledCodeNotANoop) {
    Runtime rt;
    pushInterpretedOuterFrame(rt);
    ASSERT_EQ(rt.frameCount(), 1);

    ObjFunction* methodFn = rt.memoryManager().create<ObjFunction>();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(methodFn));
    methodFn->arity = 0;
    methodFn->code = reinterpret_cast<void*>(&fakeCompiledDeferTarget);
    ObjClosure* method = rt.memoryManager().create<ObjClosure>(methodFn);
    // Keep method rooted through create<ObjBoundMethod> below, then
    // transfer to bound through recordDefer's own allocation — neither
    // object is ever on rt's own value stack in this test.
    rt.memoryManager().popTempRoot();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(method));
    ObjBoundMethod* bound = rt.memoryManager().create<ObjBoundMethod>(
        Value{1.0} /* dummy receiver */, method);
    rt.memoryManager().popTempRoot();
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(bound));

    FakeCallRecord record;
    g_record = &record;
    VMTestAccess::recordDefer(rt, /*frameIndex=*/0,
                              Value{static_cast<Obj*>(bound)});

    InterpretResult result = rt.runPendingDefers(0, 0);

    EXPECT_EQ(result, InterpretResult::OK);
    EXPECT_EQ(record.calls, 1)
        << "a bare call() on a compiled bound method only pushes a "
           "CallFrame and returns — nothing would ever run it, so the "
           "compiled body would silently never execute";
    EXPECT_EQ(rt.frameCount(), 1) << "the deferred call's own frame must be "
                                     "pushed and popped, not leaked";
}

// ===========================================================================
// Coroutine driver (backend/rt_abi.h kRtCall/kRtYield, QBE #530/#535)
// ===========================================================================

namespace {

int g_driverCalleeCalls = 0;
int g_driverCallerResumes = 0;
double g_driverSeenFromCallee = -1.0;
ObjClosure* g_driverCalleeClosure = nullptr;

// Runs to completion and leaves its result on top (the kRtOk contract).
int fakeDriverCallee(Runtime* rt, Value*) {
    g_driverCalleeCalls++;
    rt->push(Value{42.0});
    return kRtOk;
}

// On its first entry, pushes a resumable call to fakeDriverCallee and returns
// kRtCall — the shape a compiled function has when a call's callee may yield.
// The driver runs the callee and re-enters this frame at state 1 with the
// callee's result at the top.
int fakeDriverCaller(Runtime* rt, Value*) {
    if (rt->currentCompiledState() == 0) {
        rt->setCurrentCompiledState(1);
        rt->push(Value{static_cast<Obj*>(g_driverCalleeClosure)});
        rt->call(g_driverCalleeClosure, 0);
        return kRtCall;
    }
    g_driverCallerResumes++;
    g_driverSeenFromCallee = as<Number>(rt->peek(0));
    rt->push(Value{100.0});
    return kRtOk;
}

int fakeDriverFatal(Runtime*, Value*) { return kRtFatal; }

} // namespace

TEST(RunCompiledFrames, RunsCalleeThenReentersCallerAtItsResumePoint) {
    Runtime rt;
    ObjFunction* calleeFn = rt.memoryManager().create<ObjFunction>();
    calleeFn->arity = 0;
    calleeFn->code = reinterpret_cast<void*>(&fakeDriverCallee);
    ObjClosure* callee = rt.memoryManager().create<ObjClosure>(calleeFn);
    // callee is reached only through a raw pointer during the driver run, so
    // root it explicitly for the whole test.
    rt.memoryManager().pushTempRoot(static_cast<Obj*>(callee));

    ObjFunction* callerFn = rt.memoryManager().create<ObjFunction>();
    callerFn->arity = 0;
    callerFn->code = reinterpret_cast<void*>(&fakeDriverCaller);
    ObjClosure* caller = rt.memoryManager().create<ObjClosure>(callerFn);

    g_driverCalleeClosure = callee;
    g_driverCalleeCalls = 0;
    g_driverCallerResumes = 0;
    g_driverSeenFromCallee = -1.0;

    rt.push(Value{static_cast<Obj*>(caller)});
    ASSERT_EQ(rt.call(caller, 0), Runtime::ThrowOutcome::Pushed);

    bool ok = rt.runCompiledFrames(0);

    EXPECT_TRUE(ok);
    EXPECT_EQ(g_driverCalleeCalls, 1);
    EXPECT_EQ(g_driverCallerResumes, 1)
        << "the driver must re-enter the caller at its saved resume point, not"
           " abandon it after the callee returns";
    EXPECT_EQ(g_driverSeenFromCallee, 42.0)
        << "the callee's result must be in the caller's result slot on resume";
    EXPECT_EQ(rt.frameCount(), 0);
    Value result = rt.pop();
    ASSERT_TRUE(is<Number>(result));
    EXPECT_EQ(as<Number>(result), 100.0);
    rt.memoryManager().popTempRoot();
}

TEST(RunCompiledFrames, FatalStatusReturnsFalse) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    fn->arity = 0;
    fn->code = reinterpret_cast<void*>(&fakeDriverFatal);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);
    rt.push(Value{static_cast<Obj*>(closure)});
    ASSERT_EQ(rt.call(closure, 0), Runtime::ThrowOutcome::Pushed);

    EXPECT_FALSE(rt.runCompiledFrames(0));
}

TEST(RunCompiledFrames, EmptyRangeReturnsTrue) {
    Runtime rt;
    EXPECT_TRUE(rt.runCompiledFrames(0));
}

TEST(RtYield, OutsideACoroutineIsFatalWithoutAHandler) {
    Runtime* rt = rt_startup("", nullptr, 0);
    ASSERT_NE(rt, nullptr);
    testing::internal::CaptureStderr();
    int status = rt_yield(rt, 0);
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(status, static_cast<int>(Runtime::OpResult::Fatal));
    EXPECT_NE(err.find("Cannot yield from outside a coroutine."),
              std::string::npos);
    rt_shutdown(rt);
}
