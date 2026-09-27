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
    return 0;
}

int fakeCompiledFatal(Runtime* rt, Value* base) {
    g_record->calls++;
    g_record->rt = rt;
    g_record->base = base;
    return 1;
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
    fn->arity = 2;
    fn->code = reinterpret_cast<void*>(&fakeCompiledOk);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);

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

TEST(RtCallCompiled, NonzeroReturnIsFatalAndStillPopsTheFrame) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    fn->arity = 0;
    fn->code = reinterpret_cast<void*>(&fakeCompiledFatal);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);

    FakeCallRecord record;
    g_record = &record;

    rt.push(Value{static_cast<Obj*>(closure)});
    int framesBefore = rt.frameCount();
    int status = rt_call(&rt, 0);

    EXPECT_EQ(status, static_cast<int>(Runtime::OpResult::Fatal));
    EXPECT_EQ(rt.frameCount(), framesBefore);
    EXPECT_EQ(record.calls, 1);
}

// Proves the check can fail: an arity mismatch must reject the call before
// ever reaching the compiled code, the same as it does for an interpreted
// closure.
TEST(RtCallCompiled, ArityMismatchNeverInvokesCode) {
    Runtime rt;
    ObjFunction* fn = rt.memoryManager().create<ObjFunction>();
    fn->arity = 2;
    fn->code = reinterpret_cast<void*>(&fakeCompiledOk);
    ObjClosure* closure = rt.memoryManager().create<ObjClosure>(fn);

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

TEST(RtCapiCheckpoint, PrintsAStringAndCallsAStdlibFunction) {
    Runtime* rt = rt_startup("", nullptr, 0);
    ASSERT_NE(rt, nullptr);

    Value greeting = rt_new_string(rt, "hello from qbe");
    rt_push(rt, greeting);
    testing::internal::CaptureStdout();
    int printStatus = rt_op_print(rt);
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
