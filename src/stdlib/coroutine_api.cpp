#include "coroutine_api.h"
#include "stdlib_context.h"
#include "../container_objects.h"
#include "../exec_objects.h"
#include "../object.h"
#include "../runtime.h"
#include "../value.h"
#include "../vm_allocator.h"

// coroutine.create(fn): a new, suspended Coroutine. fn must be callable
// (spec/05-stdlib.md: a Function or BoundMethod); any other value is a
// runtime error. The call does not run fn.
static Value coroutineCreateNative(int /*argCount*/, Value* argv) {
    Value fn = argv[0];
    if (!isClosure(fn) && !isBoundMethod(fn)) {
        nativeRuntimeError("coroutine.create expects a function.");
        return from<Nil>(Nil{});
    }
    Runtime* rt = getActiveRuntime();
    ObjCoroutine* co =
        rt->memoryManager().create<ObjCoroutine>(rt->coroutineClass(), fn);
    return Value{static_cast<Obj*>(co)};
}

// co.resume(...): runs co until it yields, returns, or throws. On a resume
// fault or a throw that leaves the coroutine, Runtime::resumeCoroutine
// records the outcome and this native returns a placeholder, which
// callNative replaces with the propagated outcome.
static Value coroutineResumeNative(int argCount, Value* argv) {
    ObjCoroutine* co = asObjCoroutine(argv[-1]);
    Value result;
    if (!getActiveRuntime()->resumeCoroutine(co, argCount, &result)) {
        return from<Nil>(Nil{});
    }
    return result;
}

static Value coroutineStatusNative(int /*argCount*/, Value* argv) {
    ObjCoroutine* co = asObjCoroutine(argv[-1]);
    const char* name = "suspended";
    switch (co->state) {
    case CoroutineState::SUSPENDED:
        break;
    case CoroutineState::RUNNING:
        name = "running";
        break;
    case CoroutineState::NORMAL:
        name = "normal";
        break;
    case CoroutineState::DEAD:
        name = "dead";
        break;
    }
    return Value{static_cast<Obj*>(getActiveMM()->makeString(name))};
}

ObjClass* registerCoroutineAPI(StdlibRegistrar& reg) {
    ObjClass* klass = reg.makeClass("Coroutine");
    reg.mm().pushTempRoot(klass);
    reg.addMethod(klass, "resume", coroutineResumeNative, -1);
    reg.addMethod(klass, "status", coroutineStatusNative, 0);

    // `coroutine` is a separate module object whose single member is create.
    // Its class carries no resume/status methods: those cast the receiver to
    // an ObjCoroutine, so exposing them on the module instance would let
    // `coroutine.status()` reinterpret that instance as a coroutine.
    ObjClass* moduleClass = reg.makeClass("CoroutineModule");
    reg.mm().pushTempRoot(moduleClass);
    ObjInstance* module = reg.makeInstance(moduleClass);
    reg.mm().pushTempRoot(module);
    reg.addNativeField(module, "create", coroutineCreateNative, 1);
    reg.defineGlobalValue("coroutine", Value{static_cast<Obj*>(module)});
    reg.mm().popTempRoot(); // module
    reg.mm().popTempRoot(); // moduleClass
    reg.mm().popTempRoot(); // klass
    return klass;
}
